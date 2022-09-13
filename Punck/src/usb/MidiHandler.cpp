#include "USB.h"
#include "MidiHandler.h"
#include "VoiceManager.h"
#include "config.h"
#include "sequencer.h"

void MidiHandler::DataIn()
{

}

void MidiHandler::DataOut()
{
	// Handle incoming midi command here
	uint8_t* outBuffBytes = reinterpret_cast<uint8_t*>(outBuff);

	if (!partialSysEx && outBuffCount == 4) {
		midiEvent(*outBuff);

	} else if (partialSysEx || (outBuffBytes[1] == 0xF0 && outBuffCount > 3)) {		// Sysex
		// sysEx will be padded when supplied by usb - add only actual sysEx message bytes to array
		uint16_t sysExCnt = partialSysEx ? 1 : 2;		// If continuing a long sysex command only ignore first (size) byte

		uint16_t i;
		for (i = partialSysEx ? sysExCount : 0; i < sysexMaxSize; ++i) {
			if (outBuffBytes[sysExCnt] == 0xF7) {
				partialSysEx = false;
				break;
			}
			if (sysExCnt >= outBuffCount) {				// Long SysEx command will be received in multiple packets
				partialSysEx = true;
				break;
			}

			sysEx[i] = outBuffBytes[sysExCnt++];

			// remove 1 byte padding at the beginning of each 32 bit word
			if (sysExCnt % 4 == 0) {
				++sysExCnt;
			}
		}
		sysExCount = i;
		if (!partialSysEx) {
			ProcessSysex();
		}
	}
}


uint32_t MidiHandler::ConstructSysEx(uint8_t* dataBuffer, uint32_t dataLen, uint8_t* headerBuffer, uint32_t headerLen, bool noSplit)
{
	// Constructs a Sysex packet: data split into 4 byte words, each starting with appropriate sysex header byte
	// Bytes in sysEx commands must have upper nibble = 0 (ie only 0-127 values) so double length and split bytes into nibbles (unless noSplit specified)

	if (!noSplit) {
		dataLen *= 2;
	}
	uint32_t pos = 0;

	sysExOut[pos++] = 0x04;					// 0x4	SysEx starts or continues
	sysExOut[pos++] = 0xF0;

	// header must be 7 bit values so no need to split
	for (uint32_t i = 0; i < headerLen; ++i) {
		sysExOut[pos++] = headerBuffer[i];
		if (pos % 4 == 0) {
			sysExOut[pos++] = 0x04;			// 0x4	SysEx starts or continues
		}
	}

	bool lowerNibble = true;
	for (uint32_t i = 0; i < dataLen; ++i) {
		if (noSplit) {
			sysExOut[pos++] = dataBuffer[i];
		} else {
			if (lowerNibble) {
				sysExOut[pos++] = dataBuffer[i / 2] & 0xF;
			} else {
				sysExOut[pos++] = dataBuffer[i / 2] >> 4;
			}
			lowerNibble = !lowerNibble;
		}

		// add 1 byte padding at the beginning of each 32 bit word to indicate length of remaining message + 0xF7 termination byte
		if (pos % 4 == 0) {
			uint32_t rem = dataLen - i;

			if (rem == 3) {
				sysExOut[pos++] = 0x07;		// 0x7	SysEx ends with following three bytes.
			} else if (rem == 2) {
				sysExOut[pos++] = 0x06;		// 0x6	SysEx ends with following two bytes.
			} else if (rem == 1) {
				sysExOut[pos++] = 0x05;		// 0x5	Single-byte System Common Message or SysEx ends with following single byte.
			} else {
				sysExOut[pos++] = 0x04;		// 0x4	SysEx starts or continues
			}
		}
	}
	sysExOut[pos++] = 0xF7;
	return pos;
}


uint32_t MidiHandler::ReadCfgSysEx()
{
	// Converts a Configuration encoded Sysex packet to a regular byte array (two bytes are header)
	// Data split into 4 byte words, each starting with appropriate sysEx header byte
	for (uint32_t i = 2; i < sysExCount; ++i) {
		uint32_t idx = (i / 2) - 1;
		if (i % 2 == 0) {
			config.configBuffer[idx] = sysEx[i];
		} else {
			config.configBuffer[idx] += sysEx[i] << 4;
		}
	}
	return (sysExCount / 2) - 1;
}


void MidiHandler::ProcessSysex()
{
	enum sysExCommands {GetVoiceConfig = 0x1C, SetVoiceConfig = 0x2C, GetSequence = 0x1B, SetSequence = 0x2B};

	// Check if SysEx contains read config command
	if (sysEx[0] == GetVoiceConfig && sysEx[1] < VoiceManager::voiceCount) {
		VoiceManager::Voice voice = (VoiceManager::Voice)sysEx[1];

		// Insert header data
		config.configBuffer[0] = GetVoiceConfig;
		config.configBuffer[1] = voice;

		uint8_t* cfgBuffer = nullptr;
		uint32_t bytes = voiceManager.noteMapper[voice].drumVoice->SerialiseConfig(&cfgBuffer);

		uint32_t len = ConstructSysEx(cfgBuffer, bytes, config.configBuffer, 2, false);
		len = ((len + 3) / 4) * 4;			// round up output size to multiple of 4
		usb->SendData(sysExOut, len, inEP);

	}

	if (sysEx[0] == SetVoiceConfig && sysEx[1] < VoiceManager::voiceCount) {
		uint32_t bytes = ReadCfgSysEx();
		voiceManager.noteMapper[sysEx[1]].drumVoice->StoreConfig(config.configBuffer, bytes);

	}

	if (sysEx[0] == GetSequence) {
		// if passed 127 then requesting currently active sequence
		uint8_t seq = sysEx[1];
		uint8_t bar = sysEx[2];
		if (seq == getActiveSequence) {
			seq = sequencer.activeSequence;
		}
		auto seqInfo = sequencer.GetSeqInfo(seq);

		// Insert header data
		config.configBuffer[0] = GetSequence;
		config.configBuffer[1] = seq;					// sequence
		config.configBuffer[2] = seqInfo.beatsPerBar;	// beats per bar
		config.configBuffer[3] = seqInfo.bars;			// bars
		config.configBuffer[4] = bar;					// bar number

		uint8_t* cfgBuffer = nullptr;
		uint32_t bytes = sequencer.GetBar(&cfgBuffer, seq, bar);
		uint32_t len = ConstructSysEx(cfgBuffer, bytes, config.configBuffer, 5, true);
		len = ((len + 3) / 4) * 4;			// round up output size to multiple of 4
		usb->SendData(sysExOut, len, inEP);
	}

	if (sysEx[0] == SetSequence) {
		//uint32_t bytes = ReadCfgSysEx();

		// Header information
		uint32_t seq = sysEx[1];				// sequence
		uint32_t beatsPerBar = sysEx[2];		// beats per bar
		uint32_t bars = sysEx[3];				// bars
		uint32_t bar = sysEx[4];				// bar number

		sequencer.StoreConfig(sysEx + 5, sysExCount - 5, seq, bar, beatsPerBar, bars);

	}
}


void MidiHandler::midiEvent(const uint32_t data)
{
	auto midiData = MidiData(data);
	MidiNote midiNote(midiData.db1, midiData.db2);

	switch (midiData.msg) {
	case NoteOff:
		break;

	case NoteOn:
		voiceManager.NoteOn(midiNote);
		break;

	case PitchBend:
		pitchBend = static_cast<uint32_t>(midiData.db1) + (midiData.db2 << 7);
		break;
	}

}


void MidiHandler::serialHandler(uint32_t data)
{
	Queue[QueueWrite] = data;
	QueueSize++;
	QueueWrite = (QueueWrite + 1) % MIDIQUEUESIZE;

	MIDIType type = static_cast<MIDIType>(Queue[QueueRead] >> 4);
	uint8_t channel = Queue[QueueRead] & 0x0F;

	//NoteOn = 0x9, NoteOff = 0x8, PolyPressure = 0xA, ControlChange = 0xB, ProgramChange = 0xC, ChannelPressure = 0xD, PitchBend = 0xE, System = 0xF
	while ((QueueSize > 2 && (type == NoteOn || type == NoteOff || type == PolyPressure ||  type == ControlChange ||  type == PitchBend)) ||
			(QueueSize > 1 && (type == ProgramChange || type == ChannelPressure))) {

		MidiData event;
		event.chn = channel;
		event.msg = (uint8_t)type;

		QueueInc();
		event.db1 = Queue[QueueRead];
		QueueInc();
		if (type == ProgramChange || type == ChannelPressure) {
			event.db2 = 0;
		} else {
			event.db2 = Queue[QueueRead];
			QueueInc();
		}

		midiEvent(event.data);

		type = static_cast<MIDIType>(Queue[QueueRead] >> 4);
		channel = Queue[QueueRead] & 0x0F;
	}

	// Clock
	if (QueueSize > 0 && Queue[QueueRead] == 0xF8) {
		midiEvent(0xF800);
		QueueInc();
	}

	//	handle unknown data in queue
	if (QueueSize > 2 && type != 0x9 && type != 0x8 && type != 0xD && type != 0xE) {
		QueueInc();
	}
}


inline void MidiHandler::QueueInc() {
	QueueSize--;
	QueueRead = (QueueRead + 1) % MIDIQUEUESIZE;
}




void MidiHandler::ClassSetup(usbRequest& req)
{
}


void MidiHandler::ClassSetupData(usbRequest& req, const uint8_t* data)
{
}


/*
Byte 1									|	Byte2		|	Byte 3		|	Byte 4
Cable Number | Code Index Number (CIN)	|	MIDI_0		|	MIDI_1		|	MIDI_2

CIN		MIDI_x Size Description
0x0		1, 2 or 3	Miscellaneous function codes. Reserved for future extensions.
0x1		1, 2 or 3	Cable events. Reserved for future expansion.
0x2		2			Two-byte System Common messages like MTC, SongSelect, etc.
0x3		3			Three-byte System Common messages like SPP, etc.
0x4		3			SysEx starts or continues
0x5		1			Single-byte System Common Message or SysEx ends with following single byte.
0x6		2			SysEx ends with following two bytes.
0x7		3			SysEx ends with following three bytes.
0x8		3			Note-off
0x9		3			Note-on
0xA		3			Poly-KeyPress
0xB		3			Control Change
0xC		2			Program Change
0xD		2			Channel Pressure
0xE		3			PitchBend Change
0xF		1			Single Byte
*/



