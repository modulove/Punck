#include <configManager.h>
#include "USB.h"
#include "CDCHandler.h"
#include "ExtFlash.h"
#include "FatTools.h"
#include "Samples.h"
#include "VoiceManager.h"
#include "ff.h"

uint32_t flashBuff[8192];
uint32_t* heapVal;		// Debug

void CDCHandler::DataIn()
{
	if (inBuffSize > 0 && inBuffSize % USB::ep_maxPacket == 0) {
		inBuffSize = 0;
		EndPointTransfer(Direction::in, inEP, 0);				// Fixes issue transmitting an exact multiple of max packet size (n x 64)
	}
}


// As this is called from an interrupt assign the command to a variable so it can be handled in the main loop
void CDCHandler::DataOut()
{
	// Check if sufficient space in command buffer
	const uint32_t newCharCnt = std::min(outBuffCount, maxCmdLen - 1 - buffPos);

	strncpy(&comCmd[buffPos], (char*)outBuff, newCharCnt);
	buffPos += newCharCnt;

	// Check if cr has been sent yet
	if (comCmd[buffPos - 1] == 13 || comCmd[buffPos - 1] == 10 || buffPos == maxCmdLen - 1) {
		comCmd[buffPos - 1] = '\0';
		cmdPending = true;
		buffPos = 0;
	}
}


void CDCHandler::ActivateEP()
{
	EndPointActivate(USB::CDC_In,   Direction::in,  EndPointType::Bulk);			// Activate CDC in endpoint
	EndPointActivate(USB::CDC_Out,  Direction::out, EndPointType::Bulk);			// Activate CDC out endpoint
	EndPointActivate(USB::CDC_Cmd,  Direction::in,  EndPointType::Interrupt);		// Activate Command IN EP

	EndPointTransfer(Direction::out, USB::CDC_Out, USB::ep_maxPacket);
}


void CDCHandler::ClassSetup(usbRequest& req)
{
	if (req.RequestType == DtoH_Class_Interface && req.Request == GetLineCoding) {
		SetupIn(req.Length, (uint8_t*)&lineCoding);
	}

	if (req.RequestType == HtoD_Class_Interface && req.Request == SetLineCoding) {
		// Prepare to receive line coding data in ClassSetupData
		usb->classPendingData = true;
		EndPointTransfer(Direction::out, 0, req.Length);
	}
}


void CDCHandler::ClassSetupData(usbRequest& req, const uint8_t* data)
{
	// ClassSetup passes instruction to set line coding - this is the data portion where the line coding is transferred
	if (req.RequestType == HtoD_Class_Interface && req.Request == SetLineCoding) {
		lineCoding = *(LineCoding*)data;
	}
}


int32_t CDCHandler::ParseInt(const std::string_view cmd, const char precedingChar, const int32_t low = 0, const int32_t high = 0) {
	int32_t val = -1;
	const int8_t pos = cmd.find(precedingChar);		// locate position of character preceding
	if (pos >= 0 && std::strspn(&cmd[pos + 1], "0123456789-") > 0) {
		val = std::stoi(&cmd[pos + 1]);
	}
	if (high > low && (val > high || val < low)) {
		printf("Must be a value between %ld and %ld\r\n", low, high);
		return low - 1;
	}
	return val;
}

// Timing debug variables
extern uint32_t loopTime, maxLoopTime, outputTime, maxOutputTime, reverbTime, maxReverbTime;

void CDCHandler::ProcessCommand()
{
	if (!cmdPending) {
		return;
	}

	std::string_view cmd {comCmd};

	if (cmd.compare("help") == 0) {
		usb->SendString("Mountjoy Punck\r\n"
				"\r\nSupported commands:\r\n"
				"saveconfig  -  Save configuration to internal memory\r\n"
				"restoreconfig  Restore saved configuration\r\n"
				"clearconfig -  Erase configuration and restart\r\n"
				"dirdetails  -  Print detailed file list for root directory\r\n"
				"dir         -  Print list of all files and their directories\r\n"
				"samplelist  -  Show details of all samples found in flash\r\n"
				"midimap     -  Display MIDI note mapping\r\n"
				"midichn:x   -  Set drum map MIDI channel (0 = omni)\r\n"
				"pitchchn:v,c   Set chromatic channel c (0 = off) for voice v (1-7, order as midimap)\r\n"
				"mix         -  Show master bus (compressor, clipper) and ducking settings\r\n"
				"mix:i,v     -  Set mix setting i to value v (saveconfig to keep)\r\n"
				"cpu         -  Audio CPU load and compressor / clipper meter\r\n"
				"crash       -  Reset reason and last fault (the module restarts after a fault); crash:0 clears\r\n"
				"status      -  Switch position, sound engines, held buttons\r\n"
				"pots        -  Pot positions\r\n"
				"cfg         -  Settings storage info (settings save automatically 3 s after a change)\r\n"
				"engine:v,e  -  Set engine e (from 1) on voice v (1 kick, 2 snare, 3 hats, 6 toms/bass)\r\n"
				"play:v,vel,o   Trigger voice v (1-7) with velocity and note offset\r\n"
				"tone:f,dB   -  Test sine (dB re clipper ceiling) replacing the output; tone:0 = off\r\n"
				"reboot      -  Reboot device\r\n"
				"leds:x      -  Set all LEDs to brightness from 1-100%\r\n"
				"revertleds  -  Reset all LEDs\r\n"
				"\r\n"
				"Flash Tools:\r\n"
				"------------\r\n"
				"readreg     -  Print QSPI flash status registers\r\n"
				"flashid     -  Print flash manufacturer and device IDs\r\n"
				"printflash:A   Print 512 bytes of flash (A = decimal address)\r\n"
				"printcluster:A Print 2048 bytes of cluster address A (>=2)\r\n"
				"clusterchain   List chain of FAT clusters\r\n"
				"cacheinfo   -  Summary of unwritten changes in header cache\r\n"
				"cachechanges   Show all bytes changed in header cache\r\n"
				"flushcache  -  Flush any changed data in cache to flash\r\n"
				"eraseflash  -  Erase all sample storage flash data\r\n"
				"format      -  Format sample storage flash\r\n"
				"eraseblock:A   Erase block of memory (8192 bytes in dual flash mode)\r\n"

		);


#if (USB_DEBUG)
	} else if (cmd.compare("usbdebug") == 0) {					// Activate USB debugging
		extern volatile bool debugStart;
		debugStart = true;
#endif



	} else if (cmd.compare("timing") == 0) {					// Print timing debug info
#if (TIMINGDEBUG)
		printf("Timings: Loop: %ld, Max: %ld, Output: %ld, Max: %ld, Underrun: %ld\r\n",
				loopTime, maxLoopTime, outputTime, maxOutputTime, i2sUnderrun);
		for (auto note : voiceManager.noteMapper) {
			switch (note.voice) {
			case VoiceManager::kick:
				printf("Kick    ");
				break;
			case VoiceManager::snare:
				printf("Snare   ");
				break;
			case VoiceManager::hihat:
				printf("Hi Hats ");
				break;
			case VoiceManager::samplerA:
				printf("Sample A");
				break;
			case VoiceManager::samplerB:
				printf("Sample B");
				break;
			case VoiceManager::toms:
				printf("Toms    ");
				break;
			case VoiceManager::claps:
				printf("Claps   ");
				break;
			}
			printf(": %ld\r\n", note.drumVoice->debugMaxTime);
		}
		printf("Reverb current: %ld maximum: %ld\r\n", reverbTime, maxReverbTime);
#else
		printf("I2C Underrun: %ld\r\n", i2sUnderrun);
#endif

	} else if (cmd.compare("crash") == 0 || cmd.compare("crash:0") == 0) {	// Last fault (restart) and reset reason
		const uint32_t r = resetFlags;
		printf("Reset reason:%s%s%s%s%s%s%s\r\n", (r & RCC_RSR_PORRSTF) ? " power-on" : "", (r & RCC_RSR_BORRSTF) ? " brownout" : "",
				(r & RCC_RSR_PINRSTF) ? " pin" : "", (r & RCC_RSR_SFTRSTF) ? " software/fault" : "",
				(r & RCC_RSR_IWDG1RSTF) ? " watchdog" : "", (r & RCC_RSR_WWDG1RSTF) ? " window-watchdog" : "",
				(r & RCC_RSR_LPWRRSTF) ? " low-power" : "");
		static constexpr const char* types[] = {"-", "hard fault", "memory fault", "bus fault", "usage fault"};
		printf("Faults since power on: %lu", crashRecord.count);
		if (crashRecord.count > 0) {
			printf("  last: %s at pc 0x%08lx lr 0x%08lx after %lu ms  CFSR 0x%08lx HFSR 0x%08lx MMFAR 0x%08lx BFAR 0x%08lx",
					types[crashRecord.type < 5 ? crashRecord.type : 0], crashRecord.pc, crashRecord.lr, crashRecord.uptimeMs,
					crashRecord.cfsr, crashRecord.hfsr, crashRecord.mmfar, crashRecord.bfar);
		}
		printf("\r\n");
		if (cmd.compare("crash:0") == 0) {
			crashRecord.count = 0;
			crashRecord.type = 0;
			printf("Fault record cleared\r\n");
		}

	} else if (cmd.compare("crashtest") == 0) {					// Deliberate fault: checks that the module records it and restarts
		printf("Faulting now: the module restarts, then 'crash' shows the record\r\n");
		const uint32_t start = SysTickVal;
		while (SysTickVal - start < 50) {}						// let the line go out over USB first
		__builtin_trap();

	} else if (cmd.compare("cpu") == 0) {						// Audio CPU load and master bus meter
		VoiceManager::Meter m;
		voiceManager.GetMeter(m);
		printf("CPU avg: %.1f%%  max: %.1f%%  underruns: %lu  uptime: %lu s  crashes: %lu\r\n", m.cpuAvg, m.cpuMax, i2sUnderrun,
				SysTickVal / 1000, crashRecord.count);
		printf("Audio rate: %lu/s (48000)  I2S restarts: %lu%s  USB EP0 aborts: %lu  clock %lu MHz\r\n", audioRate, i2sRestarts,
				i2sRestarts ? "  <- rate watchdog fired" : "", usb->ep0Aborts, SystemCoreClock / 1000000);
		if (i2sRestarts) {
			printf("Last bad rate: %lu/s\r\n", lastBadRate);
		}
		{
			const auto& n = voiceManager.nanEvents;
			uint32_t total = 0;
			for (auto v : n) {
				total += v;
			}
			if (total) {
				printf("NaN/inf recoveries: kick %lu snare %lu hats %lu samples %lu toms %lu claps %lu reverb %lu bus %lu out %lu\r\n",
						n[0], n[1], n[2], n[3] + n[4], n[5], n[6], n[VoiceManager::nanReverb], n[VoiceManager::nanBus], n[VoiceManager::nanOut]);
			}
		}
		printf("Comp reduction max: %.1f dB  into clipper: %+.1f dB  output peak: %+.1f dBFS\r\n", m.reductionDb, m.clipDb, m.peakDb);
		static constexpr const char* blocks[] = {"kick", "snare", "hats", "samples", "-", "toms", "claps", "reverb", "master", "trig"};
		printf("Cycles avg/max per sample (budget %lu):", SystemCoreClock / 48000);
		for (uint32_t i = 0; i < VoiceManager::profCount; ++i) {
			if (i != VoiceManager::Voice::samplerB) {
				printf(" %s %.0f/%.0f", blocks[i], voiceManager.profAvg[i], voiceManager.profMax[i]);
				voiceManager.profMax[i] = 0.0f;
			}
		}
		printf("\r\n");

	} else if (cmd.compare("status") == 0) {					// Switch position and sound engines
		static constexpr const char* kickNames[] = {"classic", "deep", "punch", "fm", "rumble"};
		static constexpr const char* snareNames[] = {"classic", "808", "909", "rim", "modal", "string", "bongo"};
		static constexpr const char* tomNames[] = {"toms", "bass", "pluck", "fm", "va", "fold"};
		static constexpr const char* hatNames[] = {"classic", "808", "909"};
		printf("Switch: %s\r\n", voiceManager.ButtonModeName());
		const uint8_t k = voiceManager.kickPlayer.GetEngine(), s = voiceManager.snarePlayer.GetEngine(), h = voiceManager.hihatPlayer.GetEngine();
		const uint8_t t = voiceManager.tomsPlayer.GetEngine();
		printf("Kick  engine %d (%s)\r\nSnare engine %d (%s)\r\nHats  engine %d (%s)\r\nToms  engine %d (%s)  (voice 6, MIDI only)\r\n",
				k + 1, kickNames[k % 5], s + 1, snareNames[s % 7], h + 1, hatNames[h % 3], t + 1, tomNames[t % 6]);
		printf("Buttons held:");
		for (auto& nm : voiceManager.noteMapper) {
			if (nm.trigger.ButtonHeld()) {
				printf(" %d", nm.voice + 1);
			}
		}
		printf("\r\n");

	} else if (cmd.compare("cfg") == 0) {						// Settings storage info
		uint32_t latest = 0;
		const uint32_t freePos = configManager.FindRecords(latest);
		printf("Settings: newest record at %lu, free from %lu of %lu bytes; saves since boot %lu, erases %lu; save pending: %s\r\n",
				latest, freePos, configManager.sectorSize, configManager.saveCount, configManager.eraseCount,
				configManager.scheduleSave ? "yes" : "no");

	} else if (cmd.compare("pots") == 0) {						// Pot positions (0 - 100 %)
		static constexpr const char* names[] = {"Kick attack", "Kick decay", "Kick level", "Snare filter", "Snare tuning", "Snare level",
				"Sample A level", "Sample B level", "Tempo", "Hats decay", "Hats level", "Sample A voice", "Sample A speed",
				"Sample B voice", "Sample B speed"};
		for (uint32_t i = 0; i < ADC1_BUFFER_LENGTH + ADC2_BUFFER_LENGTH; ++i) {
			printf("%-15s %5.1f %%\r\n", names[i], ADC_array[i] / 655.35f);
		}

	} else if (cmd.compare(0, 7, "engine:") == 0) {				// engine:v,e  set engine e (1 = first, as blinked) on voice v (1 kick, 2 snare, 3 hats)
		int v = 0, e = 0;
		if (sscanf(cmd.data() + 7, "%d,%d", &v, &e) == 2 && v >= 1 && v <= VoiceManager::Voice::count && e >= 1) {
			voiceManager.RequestEngine(v - 1, e - 1);
			printf("Voice %d engine %d\r\n", v, e);
		} else {
			printf("Format: engine:voice,engine  (voice 1 kick, 2 snare, 3 hats; engine from 1)\r\n");
		}

	} else if (cmd.compare(0, 5, "play:") == 0) {				// play:v[,velocity 1-127[,note offset]][;v,...]  trigger voices (test recordings)
		const char* p = cmd.data() + 5;
		while (p && *p) {
			int v = 0, vel = 100, offset = 0;
			const int n = sscanf(p, "%d,%d,%d", &v, &vel, &offset);
			if (n >= 1 && v >= 1 && v <= VoiceManager::Voice::count) {
				NoteMapper& nm = voiceManager.noteMapper[v - 1];
				const uint32_t range = nm.midiHigh - nm.midiLow + 1;
				if (nm.drumVoice) {
					nm.drumVoice->QueuePlay(nm.voiceIndex, std::min((uint32_t)std::max(offset, 0), range - 1), range, std::clamp(vel, 1, 127) / 127.0f);
				}
			}
			p = strchr(p, ';');
			if (p) {
				++p;
			}
		}

	} else if (cmd.compare(0, 5, "tone:") == 0) {				// tone:freq,dB  test sine replacing the output (dB re clipper ceiling), tone:0 = off
		char* end = nullptr;
		const float f = strtof(cmd.data() + 5, &end);
		const float db = (end && *end == ',') ? strtof(end + 1, nullptr) : -20.0f;
		voiceManager.SetTestTone(f, db);
		if (f > 0.0f) {
			printf("Test tone %.0f Hz at %.1f dB\r\n", f, db);
		} else {
			printf("Test tone off\r\n");
		}

	} else if (cmd.compare(0, 3, "mix") == 0) {					// Show or set master bus / ducking parameters: mix or mix:index,value
		static constexpr const char* names[] = {"duckDrums", "duckSamplers", "duckReverb", "duckHoldMs", "duckReleaseMs", "kickReverbSend",
				"compOn", "compThreshold dB", "compRatio", "compAttack ms", "compRelease ms", "compKnee dB", "compMakeup dB", "compMix",
				"compScHpf Hz", "compKick", "clipMode 0 tanh 1 clean 2 hard 3 warm", "clipDrive dB", "outTrim dB", "inputTrim dB",
				"keyRoot -1 off, 0 C .. 11 B", "keyScale 0 min 1 dor 2 phr ..", "tune kick 0 off 1 root 2 r+5 3 scale 4 pent",
				"tune snare", "tune hats", "tune sample A", "tune sample B", "tune toms / bass", "tune claps"};
		constexpr uint32_t count = sizeof(VoiceManager::MixConfig) / sizeof(float);
		static_assert(sizeof(names) / sizeof(names[0]) == count, "mix names out of sync with MixConfig");
		float* values = reinterpret_cast<float*>(&voiceManager.mixConfig);
		if (cmd.size() > 4 && cmd[3] == ':') {
			char* end = nullptr;
			const long idx = strtol(cmd.data() + 4, &end, 10);	// nano.specs has no scanf float support: parse by hand
			if (end && *end == ',' && idx >= 0 && idx < (long)count) {
				values[idx] = strtof(end + 1, nullptr);
				voiceManager.StoreMixConfig(nullptr, 0);		// Recalculate coefficients
				configManager.ScheduleSave();
			} else {
				printf("Format: mix:index,value\r\n");
			}
		}
		for (uint32_t i = 0; i < count; ++i) {
			printf("%2lu %-30s %8.2f\r\n", i, names[i], values[i]);
		}

	} else if (cmd.compare("resettiming") == 0) {				// Print timing debug info
		loopTime = 0;
		maxLoopTime = 0;
		outputTime = 0;
		maxOutputTime = 0;
		i2sUnderrun = 0;
		reverbTime = 0;
		maxReverbTime = 0;

		for (auto note : voiceManager.noteMapper) {
			note.drumVoice->debugMaxTime = 0;
		}
		printf("Reset Debug Timings\r\n");



	} else if (cmd.compare("saveconfig") == 0) {				// Serialise config to internal flash
		printf("Saving config\r\n");
		configManager.SaveConfig();


	} else if (cmd.compare(0, 5, "leds:") == 0) {				// Percentage brightness of all leds
		const int32_t percent = ParseInt(cmd, ':', 0, 100);
		if (percent >= 0) {
			voiceManager.SetAllLeds((float)percent / 100.0f);
		}


	} else if (cmd.compare("revertleds") == 0) {				// Revert leds to previous values
		voiceManager.RestoreAllLeds();


	} else if (cmd.compare("restoreconfig") == 0) {				// Restore config from internal flash
		printf("Restoring config\r\n");
		configManager.RestoreConfig();


	} else if (cmd.compare("clearconfig") == 0) {				// Erase config from internal flash
		printf("Clearing config and restarting ...\r\n");
		configManager.SaveConfig(configManager.eraseConfig);
		DelayMS(10);
		Reboot();

	} else if (cmd.compare("reboot") == 0) {					// Restart
		printf("Rebooting ...\r\n");
		DelayMS(10);
		Reboot();

	} else if (cmd.compare(0, 8, "midichn:") == 0) {			// Set MIDI channel
		const int32_t midichannel = ParseInt(cmd, ':', 0, 16);
		if (midichannel >= 0) {
			voiceManager.midiChannel = midichannel;
			configManager.SaveConfig();
			printf("MIDI Channel: %d\r\n", voiceManager.midiChannel);
		}

	} else if (cmd.compare(0, 9, "pitchchn:") == 0) {			// Set chromatic MIDI channel for a voice
		int v = 0, c = -1;
		if (sscanf(cmd.data() + 9, "%d,%d", &v, &c) == 2 && v >= 1 && v <= VoiceManager::Voice::count && c >= 0 && c <= 16) {
			voiceManager.noteMapper[v - 1].pitchChannel = c;
			configManager.SaveConfig();
			printf("Voice %d pitch channel: %d\r\n", v, c);
		} else {
			printf("Format: pitchchn:voice,channel  (voice 1-7, channel 0-16)\r\n");
		}

	} else if (cmd.compare("midimap") == 0) {					// Display MIDI note mapping
		printf("MIDI mapping: drum channel: %d (0 = omni); chromatic note %d = base pitch\r\n", voiceManager.midiChannel, VoiceManager::pitchRefNote);
		printf("Voice      Low High  PitchChn\r\n");
		for (auto note : voiceManager.noteMapper) {
			switch (note.voice) {
			case VoiceManager::kick:
				printf("Kick    ");
				break;
			case VoiceManager::snare:
				printf("Snare   ");
				break;
			case VoiceManager::hihat:
				printf("Hi Hats ");
				break;
			case VoiceManager::samplerA:
				printf("Sample A");
				break;
			case VoiceManager::samplerB:
				printf("Sample B");
				break;
			case VoiceManager::toms:
				printf("Toms    ");
				break;
			case VoiceManager::claps:
				printf("Claps   ");
				break;
			}
			printf(" : %3d, %3d   %2d\r\n", note.midiLow, note.midiHigh, note.pitchChannel);
		}


	} else if (cmd.compare("readreg") == 0) {					// Read QSPI register
		usb->SendString("Status register 1: " + std::to_string(extFlash.ReadStatus(ExtFlash::readStatusReg1)) +
				"\r\nStatus register 2: " + std::to_string(extFlash.ReadStatus(ExtFlash::readStatusReg2)) +
				"\r\nStatus register 3: " + std::to_string(extFlash.ReadStatus(ExtFlash::readStatusReg3)) + "\r\n");
		extFlash.MemoryMapped();


	} else if (cmd.compare("flashid") == 0) {					// Get manufacturer and device ID
		const uint32_t flashID = extFlash.GetID();
		auto flashBytes = reinterpret_cast<const uint8_t*>(&flashID);
		if (dualFlashMode) {
			// In dual flash mode results from each device are interleaved
			printf("Flash 1: Manufacturer ID: %#04x; Device ID: %#04x\r\n"
				   "Flash 2: Manufacturer ID: %#04x; Device ID: %#04x\r\n", flashBytes[0], flashBytes[2], flashBytes[1], flashBytes[3]);
		} else {
			printf("Manufacturer ID: %#04x; Device ID: %#04x\r\n", flashBytes[0], flashBytes[1]);
		}
		extFlash.MemoryMapped();


	} else if (cmd.compare("switchflash") == 0) {				// Switch Flash Device
		const uint16_t flashBank = (QUADSPI->CR & QUADSPI_CR_FSEL) == 0 ? 2 : 1;
		QUADSPI->CR &= ~QUADSPI_CR_EN;
		uint32_t now = SysTickVal;
		while (now + 2 > SysTickVal) {};
		if (flashBank == 2) {
			QUADSPI->CR |= QUADSPI_CR_FSEL;
		} else {
			QUADSPI->CR &= ~QUADSPI_CR_FSEL;
		}
		QUADSPI->CR |= QUADSPI_CR_EN;
		extFlash.Init(false);
		printf("Flash bank switched to %d\r\n", flashBank);


	} else if (cmd.compare("format") == 0) {					// Format Flash storage with FAT
		printf("Formatting flash:\r\n");
		fatTools.Format();
		extFlash.MemoryMapped();

	} else if (cmd.compare("samplelist") == 0) {				// Prints sample list
		uint32_t pos = 0;

		printf("Num Name          Bytes    Rate Bits Channels Valid Address    Seconds Volume\r\n");

		while (voiceManager.samples.sampleList[pos].name[0] != 0) {
			printf("%3lu %.11s %7lu %7lu %3u%1s %8u %s     0x%08x %.3f  %.2f\r\n",
					pos,
					voiceManager.samples.sampleList[pos].name,
					voiceManager.samples.sampleList[pos].size,
					voiceManager.samples.sampleList[pos].sampleRate,
					voiceManager.samples.sampleList[pos].byteDepth * 8,
					voiceManager.samples.sampleList[pos].dataFormat == 3 ? "f" : " ",		// floating point format
					voiceManager.samples.sampleList[pos].channels,
					voiceManager.samples.sampleList[pos].valid ? "Y" : " ",
					(unsigned int)voiceManager.samples.sampleList[pos].startAddr,
					(float)voiceManager.samples.sampleList[pos].sampleCount / voiceManager.samples.sampleList[pos].sampleRate,
					voiceManager.samples.sampleList[pos].volume
					);
			++pos;
		}
		printf("\r\n");



	} else if (cmd.compare("dirdetails") == 0) {				// Get detailed FAT directory info
		fatTools.PrintDirInfo();


	} else if (cmd.compare(0, 5, "read:") == 0) {				// Read QSPI data (format read:A where A is address)
		const int32_t address = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (address >= 0) {
			printf("Data Read: %#010lx\r\n", extFlash.FastRead(address));
		}



	//--------------------------------------------------------------------------------------------------
	// All flash commands that rely on memory mapped data to follow this guard
	} else if (extFlash.flashCorrupt) {
		printf("** Flash Corrupt **\r\n");





	} else if (cmd.compare("dir") == 0) {						// Get basic FAT directory list
		if (fatTools.noFileSystem) {
			printf("** No file System **\r\n");
		} else {
			char workBuff[256];
			strcpy(workBuff, "/");

			fatTools.InvalidateFatFSCache();					// Ensure that the FAT FS cache is updated
			fatTools.PrintFiles(workBuff);
		}



	} else if (cmd.compare("clusterchain") == 0) {			// Print used clusters with links from FAT area
		printf("Cluster | Link\r\n");
		uint32_t cluster = 0;
		while (fatTools.clusterChain[cluster]) {
			printf("%7lu   0x%04x\r\n", cluster, fatTools.clusterChain[cluster]);
			++cluster;
		}


	} else if (cmd.compare("cacheinfo") == 0) {				// Basic counts of differences between cache and Flash
		for (uint32_t blk = 0; blk < (fatCacheSectors / fatEraseSectors); ++blk) {

			// Check if block is actually dirty or clean
			uint32_t dirtyBytes = 0, firstDirtyByte = 0, lastDirtyByte = 0;
			for (uint32_t byte = 0; byte < (fatEraseSectors * fatSectorSize); ++byte) {
				uint32_t offset = (blk * fatEraseSectors * fatSectorSize) + byte;
				if (fatTools.headerCache[offset] != flashAddress[offset]) {
					++dirtyBytes;
					if (firstDirtyByte == 0) {
						firstDirtyByte = offset;
					}
					lastDirtyByte = offset;
				}
			}

			const bool blockDirty = (fatTools.dirtyCacheBlocks & (1 << blk));
			printf("Block %2lu: %s  Dirty bytes: %lu from %lu to %lu\r\n",
					blk, (blockDirty ? "dirty" : "     "), dirtyBytes, firstDirtyByte, lastDirtyByte);
		}

		// the write cache holds any blocks currently being written to to avoid multiple block erasing when writing large data
		if (fatTools.writeCacheDirty) {
			uint32_t dirtyBytes = 0, firstDirtyByte = 0, lastDirtyByte = 0;
			for (uint32_t byte = 0; byte < (fatEraseSectors * fatSectorSize); ++byte) {
				uint32_t offset = (fatTools.writeBlock * fatEraseSectors * fatSectorSize) + byte;
				if (fatTools.writeBlockCache[byte] != flashAddress[offset]) {
					++dirtyBytes;
					if (firstDirtyByte == 0) {
						firstDirtyByte = offset;
					}
					lastDirtyByte = offset;
				}
			}

			printf("Block %2lu: %s  Dirty bytes: %lu from %lu to %lu\r\n",
					fatTools.writeBlock, (fatTools.writeCacheDirty ? "dirty" : "     "), dirtyBytes, firstDirtyByte, lastDirtyByte);

		}


	} else if (cmd.compare("cachechanges") == 0) {			// List bytes that are different in cache to Flash
		uint32_t count = 0;
		uint8_t oldCache = 0, oldFlash = 0;
		bool skipDuplicates = false;

		for (uint32_t i = 0; i < (fatCacheSectors * fatSectorSize); ++i) {

			if (flashAddress[i] != fatTools.headerCache[i]) {					// Data has changed
				if (oldCache == fatTools.headerCache[i] && oldFlash == flashAddress[i] && i > 0) {
					if (!skipDuplicates) {
						printf("...\r\n");						// Print continuation mark
						skipDuplicates = true;
					}
				} else {
					printf("%5lu c: 0x%02x f: 0x%02x\r\n", i, fatTools.headerCache[i], flashAddress[i]);
				}

				oldCache = fatTools.headerCache[i];
				oldFlash = flashAddress[i];
				++count;
			} else {
				if (skipDuplicates) {
					printf("%5lu c: 0x%02x f: 0x%02x\r\n", i - 1, oldCache, oldFlash);
					skipDuplicates = false;
				}
			}

		}
		printf("Found %lu different bytes\r\n", count);


	} else if (cmd.compare(0, 11, "printflash:") == 0) {		// QSPI flash: print 512 byres of memory mapped data
		const int32_t address = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (address >= 0) {
			const uint32_t* p = (uint32_t*)(0x90000000 + address);

			for (uint32_t a = 0; a < 256; a += 4) {
				printf("%6ld: %#010lx %#010lx %#010lx %#010lx\r\n", (a * 4) + address, p[a], p[a + 1], p[a + 2], p[a + 3]);
			}
		}


	} else if (cmd.compare(0, 13, "printcluster:") == 0) {		// QSPI flash: print memory mapped data
		const int32_t cluster = ParseInt(cmd, ':', 2, 0xFFFFFF);
		if (cluster >= 2) {
			printf("Cluster %ld:\r\n", cluster);

			const uint32_t* p = (uint32_t*)(fatTools.GetClusterAddr(cluster));

			for (uint32_t a = 0; a < 512; a += 4) {
				printf("0x%08lx: %#010lx %#010lx %#010lx %#010lx\r\n", (a * 4) + (uint32_t)p, p[a], p[a + 1], p[a + 2], p[a + 3]);
			}
		}


	} else if (cmd.compare(0, 7, "setzero") == 0) {				// Set data at address to 0 [A = address; W = num words]
		const int32_t address = ParseInt(cmd, 'o', 0, 0xFFFFFF);
		if (address >= 0) {
			const uint32_t words = ParseInt(cmd, ':', 0, 0xFFFFFF);
			printf("Clearing %ld words at %ld ...\r\n", words, address);

			for (uint32_t a = 0; a < words; ++a) {
				flashBuff[a] = 0;
			}
			extFlash.WriteData(address, flashBuff, words);
			extFlash.MemoryMapped();
			printf("Finished\r\n");
		}


	} else if (cmd.compare("flushcache") == 0) {				// Flush FAT cache to Flash
		const uint8_t sectors = fatTools.FlushCache();
		printf("%i blocks flushed\r\n", sectors);
		extFlash.MemoryMapped();


	} else if (cmd.compare("eraseflash") == 0) {				// Erase all flash memory
		printf("Erasing flash. Please wait ...\r\n");
		extFlash.FullErase();
		printf("Flash erased\r\n");


	} else if (cmd.compare(0, 11, "eraseblock:") == 0) {		// Erase block of flash memory
		int32_t address = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (address >= 0) {
			address = address & ~(fatEraseSectors - 1);
			extFlash.BlockErase(address);		// Force address to 4096 byte (8192 in dual flash mode) boundary

			SCB_InvalidateDCache_by_Addr((uint32_t*)(flashAddress + address), fatEraseSectors * fatSectorSize);	// Ensure cache is refreshed after write or erase
			printf("Block at address 0x%08lx erased\r\n", address);
		}
		extFlash.MemoryMapped();


	} else if (cmd.compare(0, 12, "writesector:") == 0) {		// Write 1 sector of test data: format writesector:S [S = sector]
		const int32_t sector = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (sector >= 0) {
			printf("Writing to %ld ...\r\n", sector);

			for (uint32_t a = 0; a < 8192; ++a) {
				flashBuff[a] = a + 1;
			}
			fatTools.Write((uint8_t*)flashBuff, sector, 16);
			fatTools.FlushCache();

			printf("Finished\r\n");
		}


	} else if (cmd.compare(0, 5, "write") == 0) {				// Write test pattern to flash writeA:W [A = address; W = num words]
		const int32_t address = ParseInt(cmd, 'e', 0, 0xFFFFFF);
		if (address >= 0) {
			const uint32_t words = ParseInt(cmd, ':');
			printf("Writing %ld words to %ld ...\r\n", words, address);

			for (uint32_t a = 0; a < words; ++a) {
				flashBuff[a] = a + 1;
			}
			extFlash.WriteData(address, flashBuff, words);

			extFlash.MemoryMapped();
			printf("Finished\r\n");

		}



	} else {
		printf("Unrecognised command: %s\r\nType 'help' for supported commands\r\n", cmd.data());
	}
	cmdPending = false;

}




float CDCHandler::ParseFloat(const std::string_view cmd, const char precedingChar, const float low = 0.0f, const float high = 0.0f) {
	float val = -1.0f;
	const int8_t pos = cmd.find(precedingChar);		// locate position of character preceding
	if (pos >= 0 && std::strspn(&cmd[pos + 1], "0123456789.") > 0) {
		val = std::stof(&cmd[pos + 1]);
	}
	if (high > low && (val > high || val < low)) {
		printf("Must be a value between %f and %f\r\n", low, high);
		return low - 1.0f;
	}
	return val;
}


// Descriptor definition here as requires constants from USB class
const uint8_t CDCHandler::Descriptor[] = {
	// IAD Descriptor - Interface association descriptor for CDC class
	0x08,									// bLength (8 bytes)
	USB::IadDescriptor,						// bDescriptorType
	USB::CDCCmdInterface,					// bFirstInterface
	0x02,									// bInterfaceCount
	0x02,									// bFunctionClass (Communications and CDC Control)
	0x02,									// bFunctionSubClass
	0x01,									// bFunctionProtocol
	USB::CommunicationClass,				// String Descriptor

	// Interface Descriptor
	0x09,									// bLength: Interface Descriptor size
	USB::InterfaceDescriptor,				// bDescriptorType: Interface
	USB::CDCCmdInterface,					// bInterfaceNumber: Number of Interface
	0x00,									// bAlternateSetting: Alternate setting
	0x01,									// bNumEndpoints: 1 endpoint used
	0x02,									// bInterfaceClass: Communication Interface Class
	0x02,									// bInterfaceSubClass: Abstract Control Model
	0x01,									// bInterfaceProtocol: Common AT commands
	USB::CommunicationClass,				// iInterface

	// Header Functional Descriptor
	0x05,									// bLength: Endpoint Descriptor size
	USB::ClassSpecificInterfaceDescriptor,	// bDescriptorType: CS_INTERFACE
	0x00,									// bDescriptorSubtype: Header Func Desc
	0x10,									// bcdCDC: spec release number
	0x01,

	// Call Management Functional Descriptor
	0x05,									// bFunctionLength
	USB::ClassSpecificInterfaceDescriptor,	// bDescriptorType: CS_INTERFACE
	0x01,									// bDescriptorSubtype: Call Management Func Desc
	0x00,									// bmCapabilities: D0+D1
	0x01,									// bDataInterface: 1

	// ACM Functional Descriptor
	0x04,									// bFunctionLength
	USB::ClassSpecificInterfaceDescriptor,	// bDescriptorType: CS_INTERFACE
	0x02,									// bDescriptorSubtype: Abstract Control Management desc
	0x02,									// bmCapabilities

	// Union Functional Descriptor
	0x05,									// bFunctionLength
	USB::ClassSpecificInterfaceDescriptor,	// bDescriptorType: CS_INTERFACE
	0x06,									// bDescriptorSubtype: Union func desc
	0x00,									// bMasterInterface: Communication class interface
	0x01,									// bSlaveInterface0: Data Class Interface

	// Endpoint 2 Descriptor
	0x07,									// bLength: Endpoint Descriptor size
	USB::EndpointDescriptor,				// bDescriptorType: Endpoint
	USB::CDC_Cmd,							// bEndpointAddress
	USB::Interrupt,							// bmAttributes: Interrupt
	0x08,									// wMaxPacketSize
	0x00,
	0x10,									// bInterval

	//---------------------------------------------------------------------------

	// Data class interface descriptor
	0x09,									// bLength: Endpoint Descriptor size
	USB::InterfaceDescriptor,				// bDescriptorType:
	USB::CDCDataInterface,					// bInterfaceNumber: Number of Interface
	0x00,									// bAlternateSetting: Alternate setting
	0x02,									// bNumEndpoints: Two endpoints used
	0x0A,									// bInterfaceClass: CDC
	0x00,									// bInterfaceSubClass:
	0x00,									// bInterfaceProtocol:
	0x00,									// iInterface:

	// Endpoint OUT Descriptor
	0x07,									// bLength: Endpoint Descriptor size
	USB::EndpointDescriptor,				// bDescriptorType: Endpoint
	USB::CDC_Out,							// bEndpointAddress
	USB::Bulk,								// bmAttributes: Bulk
	LOBYTE(USB::ep_maxPacket),				// wMaxPacketSize:
	HIBYTE(USB::ep_maxPacket),
	0x00,									// bInterval: ignore for Bulk transfer

	// Endpoint IN Descriptor
	0x07,									// bLength: Endpoint Descriptor size
	USB::EndpointDescriptor,				// bDescriptorType: Endpoint
	USB::CDC_In,							// bEndpointAddress
	USB::Bulk,								// bmAttributes: Bulk
	LOBYTE(USB::ep_maxPacket),				// wMaxPacketSize:
	HIBYTE(USB::ep_maxPacket),
	0x00,									// bInterval: ignore for Bulk transfer
};


uint32_t CDCHandler::GetInterfaceDescriptor(const uint8_t** buffer) {
	*buffer = Descriptor;
	return sizeof(Descriptor);
}
