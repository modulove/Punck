#include "FatTools.h"
#include "ExtFlash.h"
#include <cstring>

FatTools fatTools;


void FatTools::InitFatFS()
{
	// Set up cache area for header data
	memcpy(headerCache, flashAddress, fatSectorSize * fatCacheSectors);

	FRESULT res = f_mount(&fatFs, fatPath, 1) ;				// Register the file system object to the FatFs module

	if (res == FR_NO_FILESYSTEM) {
		uint8_t fsWork[fatSectorSize];						// Work buffer for the f_mkfs()

		MKFS_PARM parms;									// Create parameter struct
		parms.fmt = FM_FAT | FM_SFD;						// format as FAT12/16 using SFD (Supper Floppy Drive)
		parms.n_root = 128;									// Number of root directory entries (each uses 32 bytes of storage)
		parms.align = 0;									// Default initialise remaining values
		parms.au_size = 0;
		parms.n_fat = 0;

		//f_mkfs(fatPath, &parms, fsWork, sizeof(fsWork));	// Mount FAT file system on External Flash
		res = f_mount(&fatFs, fatPath, 1);					// Register the file system object to the FatFs module
	}
}


void FatTools::Read(uint8_t* writeAddress, uint32_t readSector, uint32_t sectorCount)
{
	// If reading header data return from cache
	const uint8_t* readAddress;
	if (readSector < fatCacheSectors) {
		readAddress = &(headerCache[readSector * fatSectorSize]);
	} else {
		readAddress = flashAddress + (readSector * fatSectorSize);
	}

	memcpy(writeAddress, readAddress, fatSectorSize * sectorCount);
}


void FatTools::Write(const uint8_t* readBuff, uint32_t writeSector, uint32_t sectorCount)
{
	if (writeSector < fatCacheSectors) {
		// Update the bit array of dirty blocks [There are 8 x 512 byte sectors in a block (4096)]
		dirtyCacheBlocks |= (1 << (writeSector / fatEraseSectors));

		uint8_t* writeAddress = &(headerCache[writeSector * fatSectorSize]);
		memcpy(writeAddress, readBuff, fatSectorSize * sectorCount);
	} else {
		// Check which block is being written to
		int32_t block = writeSector / fatEraseSectors;
		if (block != writeBlock) {
			if (writeBlock > 0) {		// Write previously cached block to flash
				FlushCache();			// FIXME - add some handling to prevent constant Windows disk spam being written (access dates, indexer etc)
			}

			// Load cache with current flash values
			writeBlock = block;
			const uint8_t* readAddress = flashAddress + (block * fatEraseSectors * fatSectorSize);
			memcpy(writeBlockCache, readAddress, fatEraseSectors * fatSectorSize);
		}

		// write cache is now valid - copy newly changed values into it
		uint32_t byteOffset = (writeSector - (block * fatEraseSectors)) * fatSectorSize;		// Offset within currently block
		memcpy(&(writeBlockCache[byteOffset]), readBuff, fatSectorSize * sectorCount);
		writeCacheDirty = true;
	}

	if (writeSector >= fatHeaderSectors) {
		cacheUpdated = SysTickVal;
	}
}


void FatTools::CheckCache()
{
	// If there are dirty buffers and sufficient time has elapsed since the cache updated flag was been set flush the cache to Flash
	// FIXME - assuming only worth storing cached changes if there has been a change in the FAT data area (ie ignoring access date updates etc)
	bool headerCacheDirty = (dirtyCacheBlocks > (1 << (fatHeaderSectors / fatEraseSectors)));
	if ((headerCacheDirty || writeCacheDirty) && SysTickVal - cacheUpdated > 100)	{

		__disable_irq();					// FIXME disable all interrupts - fixes issues where USB interrupts trigger QSPI state changes during writes
		FlushCache();
		__enable_irq();						// enable all interrupts
	}
}


uint8_t FatTools::FlushCache()
{
	// Writes any dirty data in the header cache to Flash
	uint8_t blockPos = 0;
	uint8_t count = 0;
	while (dirtyCacheBlocks != 0) {
		if (dirtyCacheBlocks & (1 << blockPos)) {
			uint32_t byteOffset = blockPos * fatEraseSectors * fatSectorSize;
			if (extFlash.WriteData(byteOffset, (uint32_t*)&(headerCache[byteOffset]), 1024, true)) {
				printf("Written cache block %i\r\n", blockPos);
				++count;
			}
			dirtyCacheBlocks &= ~(1 << blockPos);
		}
		++blockPos;
	}

	// Write current working block to flash
	if (writeCacheDirty && writeBlock > 0) {
		uint32_t writeAddress = writeBlock * fatEraseSectors * fatSectorSize;
		if (extFlash.WriteData(writeAddress, (uint32_t*)writeBlockCache, (fatEraseSectors * fatSectorSize) / 4, true)) {
			printf("Written cache block %lu\r\n", writeBlock);
			++count;
		}
		writeCacheDirty = false;			// Indicates that write cache is clean
	}
	return count;
}


void FatTools::PrintDirInfo(uint32_t cluster)
{
	// Output a detailed analysis of FAT directory structure
	FATFileInfo* fatInfo;
	if (cluster == 0) {
		printf("\r\n  Attrib Cluster  Bytes    Created   Accessed Name\r\n  -----------------------------------------------\r\n");
		fatInfo = (FATFileInfo*)(headerCache + fatFs.dirbase * fatSectorSize);
	} else {
		// Byte offset of the cluster start (note cluster numbers start at 2)
		uint32_t offsetByte = (fatFs.database * fatSectorSize) + (fatClusterSize * (cluster - 2));

		// Check if cluster is in cache or not
		if (offsetByte < fatCacheSectors * fatSectorSize) {				// In cache
			fatInfo = (FATFileInfo*)(headerCache + offsetByte);
		} else {
			fatInfo = (FATFileInfo*)(flashAddress + offsetByte);		// in memory mapped flash data
		}
	}

	while (fatInfo->name[0] != 0) {
		if (fatInfo->attr == 0xF) {										// Long file name
			FATLongFilename* lfn = (FATLongFilename*)fatInfo;
			printf("%c LFN %2i                                      %s\r\n",
					(cluster == 0 ? ' ' : '>'),
					lfn->order & (~0x40),
					GetFileName(fatInfo).c_str());
		} else {
			printf("%c %s %8i %6lu %10s %10s %s\r\n",
					(cluster == 0 ? ' ' : '>'),
					(fatInfo->name[0] == 0xE5 ? "*Del*" : GetAttributes(fatInfo).c_str()),
					fatInfo->firstClusterLow,
					fatInfo->fileSize,
					FileDate(fatInfo->createDate).c_str(),
					FileDate(fatInfo->accessedDate).c_str(),
					GetFileName(fatInfo).c_str());

			// Print cluster chain
			if (fatInfo->name[0] != 0xE5 && fatInfo->fileSize > fatClusterSize) {

				bool seq = false;					// used to check for sequential blocks

				uint32_t cluster = fatInfo->firstClusterLow;
				uint16_t* clusterChain = (uint16_t*)(headerCache + (fatFs.fatbase * fatSectorSize));
				printf("  Clusters: %lu", cluster);

				while (clusterChain[cluster] != 0xFFFF) {
					if (clusterChain[cluster] == cluster + 1) {
						if (!seq) {
							printf("-");
							seq = true;
						}
					} else {
						seq = false;
						printf("%i, ", clusterChain[cluster]);
					}
					cluster = clusterChain[cluster];
				}
				if (seq) {
					printf("%lu\r\n", cluster);
				} else {
					printf("\r\n");
				}
			}
		}

		// Recursively call function to print sub directory details (ignoring directories '.' and '..' which hold current and parent directory clusters
		if ((fatInfo->attr & AM_DIR) && (fatInfo->name[0] != '.')) {
			PrintDirInfo(fatInfo->firstClusterLow);
		}
		fatInfo++;
	}
}


std::string FatTools::FileDate(uint16_t date)
{
	// Bits 0–4: Day of month, valid value range 1-31 inclusive.
	// Bits 5–8: Month of year, 1 = January, valid value range 1–12 inclusive.
	// Bits 9–15: Count of years from 1980, valid value range 0–127 inclusive (1980–2107).
	return  std::to_string( date & 0b0000000000011111) + "/" +
			std::to_string((date & 0b0000000111100000) >> 5) + "/" +
			std::to_string(((date & 0b1111111000000000) >> 9) + 1980);
}


/*
 Time Format. A FAT directory entry time stamp is a 16-bit field that has a granularity of 2 seconds. Here is the format (bit 0 is the LSB of the 16-bit word, bit 15 is the MSB of the 16-bit word).

 Bits 0–4: 2-second count, valid value range 0–29 inclusive (0 – 58 seconds).
 Bits 5–10: Minutes, valid value range 0–59 inclusive.
 Bits 11–15: Hours, valid value range 0–23 inclusive.
 */

std::string FatTools::GetFileName(FATFileInfo* fi)
{
	char cs[14];
	std::string s;
	if (fi->attr == 0xF) {									// NB - unicode characters not properly handled - just assuming ASCII char in lower byte
		FATLongFilename* lfn = (FATLongFilename*)fi;
		return std::string({lfn->name1[0], lfn->name1[2], lfn->name1[4], lfn->name1[6], lfn->name1[8],
				lfn->name2[0], lfn->name2[2], lfn->name2[4], lfn->name2[6], lfn->name2[8], lfn->name2[10],
				lfn->name3[0], lfn->name3[2], '\0'});
	} else {
		uint8_t pos = 0;
		for (uint8_t i = 0; i < 11; ++i) {
			if (fi->name[i] != 0x20) {
				cs[pos++] = fi->name[i];
			} else if (fi->name[i] == 0x20 && cs[pos - 1] == '.') {
				// do nothing
			} else {
				cs[pos++] = (fi->attr & AM_DIR) ? '\0' : '.';
			}

		}
		cs[pos] = '\0';
		return std::string(cs);
	}
}


std::string FatTools::GetAttributes(FATFileInfo* fi)
{
	char cs[6] = {(fi->attr & AM_RDO) ? 'R' : '-',
			(fi->attr & AM_HID) ? 'H' : '-',
			(fi->attr & AM_SYS) ? 'S' : '-',
			(fi->attr & AM_DIR) ? 'D' : '-',
			(fi->attr & AM_ARC) ? 'A' : '-', '\0'};
	std::string s = std::string(cs);
	return s;
}


void FatTools::InvalidateFatFSCache()
{
	// Clear the cache window in the fatFS object so that new writes will be correctly read
	fatFs.winsect = ~0;
}


// Uses FatFS to get a recursive directory listing (just shows paths and files)
void FatTools::PrintFiles(char* path)						// Start node to be scanned (also used as work area)
{
	DIR dirObj;												// Pointer to the directory object structure
	FILINFO fileInfo;										// File information structure

	FRESULT res = f_opendir(&dirObj, path);					// second parm is directory name (root)

	if (res == FR_OK) {
		for (;;) {
			res = f_readdir(&dirObj, &fileInfo);			// Read a directory item */
			if (res != FR_OK || fileInfo.fname[0] == 0) {	// Break on error or end of dir */
				break;
			}
			if (fileInfo.fattrib & AM_DIR) {				// It is a directory
				uint32_t i = strlen(path);
				sprintf(&path[i], "/%s", fileInfo.fname);
				PrintFiles(path);							// Enter the directory
				path[i] = 0;
			} else {										// It is a file
				printf("%s/%s %i bytes\n", path, fileInfo.fname, (int)fileInfo.fsize);
			}
		}
		f_closedir(&dirObj);
	}
}

/*
void CreateTestFile()
{
	FIL MyFile;												// File object

	uint32_t byteswritten, bytesread;						// File write/read counts
	uint8_t wtext[] = "This is STM32 working with FatFs";	// File write buffer
	uint8_t rtext[100];										// File read buffer

	Create and Open a new text file object with write access
	if (f_open(&MyFile, "STM32.TXT", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
		res = f_write(&MyFile, wtext, sizeof(wtext), (unsigned int*)&byteswritten);			// Write data to the text file
		if ((byteswritten != 0) && (res == FR_OK)) {
			f_close(&MyFile);																// Close the open text file
			if (f_open(&MyFile, "STM32.TXT", FA_READ) == FR_OK) {							// Open the text file object with read access
				res = f_read(&MyFile, rtext, sizeof(rtext), (unsigned int *)&bytesread);	// Read data from the text file
				if ((bytesread > 0) && (res == FR_OK)) {
					f_close(&MyFile);														// Close the open text file
				}
			}
		}
	}
}
*/