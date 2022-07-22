#include "USB.h"
#include "CDCHandler.h"
#include "ExtFlash.h"
#include "FatTools.h"
#include "ff.h"

uint32_t flashBuff[1024];

void CDCHandler::DataIn()
{

}

// As this is called from an interrupt assign the command to a variable so it can be handled in the main loop
void CDCHandler::DataOut()
{
	uint32_t maxLen = std::min((uint32_t)CDC_CMD_LEN - 1, outBuffCount);
	strncpy(comCmd, (char*)outBuff, CDC_CMD_LEN);
	comCmd[maxLen] = '\0';
	cmdPending = true;
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


int32_t CDCHandler::ParseInt(const std::string cmd, const char precedingChar, int low = 0, int high = 0) {
	int32_t val = -1;
	int8_t pos = cmd.find(precedingChar);		// locate position of character preceding
	if (pos >= 0 && std::strspn(cmd.substr(pos + 1).c_str(), "0123456789-") > 0) {
		val = stoi(cmd.substr(pos + 1));
	}
	if (high > low && (val > high || val < low)) {
		usb->SendString("Must be a value between " + std::to_string(low) + " and " + std::to_string(high) + "\r\n");
		return low - 1;
	}
	return val;
}


void CDCHandler::ProcessCommand()
{
	if (!cmdPending) {
		return;
	}

	std::string cmd = std::string(comCmd);
	if (cmd.compare("help\n") == 0) {
		usb->SendString("Mountjoy Punck\r\n"
				"\r\nSupported commands:\r\n"
				"info        -  Show diagnostic information\r\n"
				"resume      -  Resume I2S after debugging\r\n"
				"readreg     -  Print QSPI flash status registers\r\n"
				"writeA:N    -  Write sequence to flash (A = address, N = No of words decimal)\r\n"
				"writesector:S  Write 512 byte sequence via cache to sector S\r\n"
				"setzeroA:N  -  Write zero to flash (A = address, N = No of words decimal)\r\n"
				"read:A      -  Read word from flash (A = decimal address)\r\n"
				"printflash:A   Print 512 bytes of flash (A = decimal address)\r\n"
				"erasesect:A -  Erase flash sector (A = decimal address)\r\n"
				"dirdetails  -  Print detailed file list for root directory\r\n"
				"dirlist     -  Print list of all files and their directories\r\n"
				"flushcache  -  Flush any changed data in cache to flash\r\n"
				"cacheinfo   -  Show all bytes changed in header cache\r\n"
				"\r\n"
#if (USB_DEBUG)
				"usbdebug    -  Start USB debugging\r\n"
				"\r\n"
#endif
		);


#if (USB_DEBUG)
	} else if (cmd.compare("usbdebug\n") == 0) {				// Activate USB Debug
		//USBDebug = true;
		usb->SendString("Press link button to dump output\r\n");
#endif

	} else if (cmd.compare("memmap\n") == 0) {					// QSPI flash to memory mapped mode
		extFlash.MemoryMapped();
		usb->SendString("Changed to memory mapped mode\r\n");


	} else if (cmd.compare("dirlist\n") == 0) {					// Get basic FAT directory list
		char workBuff[256];
		strcpy(workBuff, "/");

		fatTools.InvalidateFATCache();							// Ensure that the FAT FS cache is updated
		fatTools.PrintFiles(workBuff);


	} else if (cmd.compare("dirdetails\n") == 0) {				// Get detailed FAT directory info
		fatTools.PrintDirInfo();


	} else if (cmd.compare("cacheinfo\n") == 0) {				// Basic counts of differences between cache and Flash
		uint32_t count = 0;
		uint8_t oldCache = 0, oldFlash = 0;
		bool skipDuplicates = false;

		for (uint32_t blk = 0; blk < (flashCacheSize / flashEraseSectors); ++blk) {

			// Check if block is actually dirty or clean
			uint32_t dirtyBytes = 0, firstDirtyByte = 0, lastDirtyByte = 0;
			for (uint32_t byte = 0; byte < (flashEraseSectors * flashSectorSize); ++byte) {
				uint32_t offset = (blk * flashEraseSectors * flashSectorSize) + byte;
				if (fatCache[offset] != ((uint8_t*)flashAddress)[offset]) {
					++dirtyBytes;
					if (firstDirtyByte == 0) {
						firstDirtyByte = offset;
					}
				}
			}

			bool blockDirty = (fatTools.dirtyCacheBlocks & (1 << blk));
			printf("Block %2lu: %s  Dirty bytes: %lu from %lu to %lu\r\n",
					blk, (blockDirty ? "dirty" : "     "), dirtyBytes, firstDirtyByte, lastDirtyByte);

		}

	} else if (cmd.compare("cachechanges\n") == 0) {				// List bytes that are different in cache to Flash
		uint32_t count = 0;
		uint8_t oldCache = 0, oldFlash = 0;
		bool skipDuplicates = false;

		for (uint32_t i = 0; i < (flashCacheSize * flashSectorSize); ++i) {
			uint8_t flashData = ((uint8_t*)flashAddress)[i];

			// Data has changed
			if (flashData != fatCache[i]) {
				if (oldCache == fatCache[i] && oldFlash == flashData && i > 0) {
					if (!skipDuplicates) {
						printf("...\r\n");						// Print continuation mark
						skipDuplicates = true;
					}
				} else {
					printf("%5lu c: 0x%02x f: 0x%02x\r\n", i, fatCache[i], flashData);
				}

				oldCache = fatCache[i];
				oldFlash = flashData;
				++count;
			} else {
				if (skipDuplicates) {
					printf("%5lu c: 0x%02x f: 0x%02x\r\n", i - 1, oldCache, oldFlash);
					skipDuplicates = false;
				}
			}

		}

		printf("Found %lu different bytes\r\n", count);



	} else if (cmd.compare(0, 11, "printflash:") == 0) {		// QSPI flash: print memory mapped data
		int address = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (address >= 0) {
			unsigned int* p = (unsigned int*)(0x90000000 + address);

			for (uint8_t a = 0; a < 128; a += 4) {
				printf("%6d: %#010x %#010x %#010x %#010x\r\n", (a * 4) + address, p[a], p[a + 1], p[a + 2], p[a + 3]);
			}
		}


	} else if (cmd.compare(0, 7, "setzero") == 0) {				// Set data at address to 0 [A = address; W = num words]
		int address = ParseInt(cmd, 'o', 0, 0xFFFFFF);
		if (address >= 0) {
			int words = ParseInt(cmd, ':');
			printf("Clearing %d words at %d ...\r\n", words, address);

			for (int a = 0; a < words; ++a) {
				flashBuff[a] = 0;
			}
			extFlash.WriteData(address, flashBuff, words, true);
			extFlash.MemoryMapped();
			printf("Finished\r\n");
		}


	} else if (cmd.compare("flushcache\n") == 0) {				// Flush FAT cache to Flash
		uint8_t sectors = fatTools.FlushCache();
		printf("%i blocks flushed\r\n", sectors);
		extFlash.MemoryMapped();


	} else if (cmd.compare(0, 10, "erasesect:") == 0) {			// Erase sector of flash memory
		int address = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (address >= 0) {
			extFlash.BlockErase(address);
			usb->SendString("Sector erased\r\n");
		}
		extFlash.MemoryMapped();


	} else if (cmd.compare("readreg\n") == 0) {					// Read QSPI register
		usb->SendString("Status register 1: " + std::to_string(extFlash.ReadStatus(ExtFlash::readStatusReg1)) +
				"\r\nStatus register 2: " + std::to_string(extFlash.ReadStatus(ExtFlash::readStatusReg2)) +
				"\r\nStatus register 3: " + std::to_string(extFlash.ReadStatus(ExtFlash::readStatusReg3)) + "\r\n");


	} else if (cmd.compare(0, 12, "writesector:") == 0) {		// Write 1 sector of test data: format writesector:S [S = sector]

		int sector = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (sector >= 0) {
			printf("Writing to %d ...\r\n", sector);

			for (uint32_t a = 0; a < flashSectorSize; ++a) {
				flashBuff[a] = a + 1;
			}
			fatTools.Write((uint8_t*)flashBuff, sector, 1);

			printf("Finished\r\n");
		}

	} else if (cmd.compare(0, 5, "write") == 0) {				// Write QSPI format writeA:W [A = address; W = num words]

		int address = ParseInt(cmd, 'e', 0, 0xFFFFFF);
		if (address >= 0) {
			int words = ParseInt(cmd, ':');
			printf("Writing %d words to %d ...\r\n", words, address);

			for (int a = 0; a < words; ++a) {
				flashBuff[a] = a + 1;
			}
			extFlash.WriteData(address, flashBuff, words, true);

			extFlash.MemoryMapped();
			printf("Finished\r\n");
		}


	} else if (cmd.compare(0, 5, "read:") == 0) {				// Read QSPI data (format read:A where A is address)
		int address = ParseInt(cmd, ':', 0, 0xFFFFFF);
		if (address >= 0) {
			printf("Data Read: %#010x\r\n", (unsigned int)extFlash.FastRead(address));
		}


	} else {
		usb->SendString("Unrecognised command: " + cmd + "Type 'help' for supported commands\r\n");
	}
	cmdPending = false;

}




float CDCHandler::ParseFloat(const std::string cmd, const char precedingChar, float low = 0.0, float high = 0.0) {
	float val = -1.0f;
	int8_t pos = cmd.find(precedingChar);		// locate position of character preceding
	if (pos >= 0 && std::strspn(cmd.substr(pos + 1).c_str(), "0123456789.") > 0) {
		val = stof(cmd.substr(pos + 1));
	}
	if (high > low && (val > high || val < low)) {
		usb->SendString("Must be a value between " + std::to_string(low) + " and " + std::to_string(high) + "\r\n");
		return low - 1.0f;
	}
	return val;
}
