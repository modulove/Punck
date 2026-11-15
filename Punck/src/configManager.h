#pragma once

#include "initialisation.h"
#include "USB.h"


static uint32_t* const addrFlashBank2Sector7 = reinterpret_cast<uint32_t*>(0x081E0000);			 // Base address of Bank 2 Sector 7, 128 Kbytes
#define FLASH_ALL_ERRORS (FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR | FLASH_SR_OPERR | FLASH_SR_RDPERR | FLASH_SR_RDSERR | FLASH_SR_SNECCERR | FLASH_SR_DBECCERR | FLASH_SR_CRCRDERR)

extern USB usb;


// Class used to store calibration settings - note this uses the Standard Peripheral Driver code
class Config {
public:
	// Version 7 (modulove fork): tagged blocks {id, length, data} so blocks can grow without invalidating saved settings
	static constexpr uint32_t configVersion = 7;
	enum BlockId : uint16_t {blockVoiceBase = 0x10, blockReverb = 0x20, blockMidiMap = 0x21, blockMix = 0x22, blockSequences = 0x23};
	static constexpr uint32_t BufferSize = 16384;
	static constexpr bool eraseConfig = true;

	// modulove: settings save themselves in the background (audio keeps running) a few seconds after the last change.
	// Records are appended in the 128 kB sector; it is only erased when full. Boot loads the newest complete record.
	static constexpr uint32_t sectorSize = 128 * 1024;
	static constexpr uint32_t autoSaveDelayMs = 3000;		// after the last change
	static constexpr uint32_t autoSaveMinGapMs = 10000;		// flash wear: at most one automatic save per 10 s

	volatile bool scheduleSave = false;
	volatile uint32_t saveBooked = 0;
	volatile bool saveNow = false;
	uint32_t lastSave = 0;
	uint32_t saveCount = 0, eraseCount = 0;					// since boot (console 'cfg')
	uint8_t configBuffer[BufferSize];	// Need a large buffer as drum sequence data is ~6k (only used by save / restore)
	uint8_t rxBuffer[2048];				// SysEx decoding and small replies in the USB interrupt (never touches a save in progress)

	void Calibrate();
	void ScheduleSave();				// called whenever a config setting is changed to schedule a save after waiting to see if any more changes are being made
	void RequestSave();					// save as soon as possible from the main loop (Tone Lab 'Save', leaving MIDI learn)
	void CheckAutoSave();				// main loop
	bool SaveConfig(bool eraseOnly = false);
	uint32_t SetConfig();				// Serialise configuration data into buffer
	void RestoreConfig();				// gets config from Flash, checks and updates settings accordingly
	uint32_t FindRecords(uint32_t& latest);	// returns the first free offset in the sector; latest = newest valid record (sectorSize = none)
	uint32_t RecordLength(const uint8_t* rec, const uint32_t maxLen);

	uint32_t AddBlock(uint32_t pos, const uint16_t id, const uint8_t* data, const uint32_t len);

	void FlashUnlock(uint8_t bank);
	void FlashLock(uint8_t bank);
	void FlashEraseSector(uint8_t Sector, uint32_t bank);
	bool FlashWaitForLastOperation(uint32_t bank);
	bool FlashProgram(uint32_t* dest_addr, uint32_t* src_addr, size_t size);
private:


};

extern Config configManager;
