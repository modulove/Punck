#pragma once

#include "initialisation.h"

extern const uint32_t* flashAddress;
static constexpr uint32_t flashBlockSize = 512;			// Default block size used by FAT
static constexpr uint32_t flashBlockCount = 31250;		// 31250 blocks of 512 bytes = 16 MBytes

class ExtFlash {
public:
	enum qspiRegister : uint8_t {pageProgram = 0x02, quadPageProgram = 0x32, readData = 0x03, fastRead = 0x6B, fastReadIO = 0xEB, writeEnable = 0x06,
		readStatusReg1 = 0x05, readStatusReg2 = 0x35, readStatusReg3 = 0x15, writeStatusReg2 = 0x31,
		sectorErase = 0x20};

	void Init();
	void MemoryMapped();
	uint8_t ReadStatus(qspiRegister r);
	void WriteEnable();
	void WriteData(uint32_t address, uint32_t* data, uint32_t words, bool checkErase = false);
	void SectorErase(uint32_t address);
	uint8_t ReadData(uint32_t address);
	uint32_t FastRead(uint32_t address);
	void CheckBusy();

	bool memMapMode = false;
private:


};

extern ExtFlash extFlash;
