#include <configManager.h>
#include <cmath>
#include "VoiceManager.h"
#include "Sequencer.h"
#include "Reverb.h"

Config configManager;

// called whenever a config setting is changed to schedule a save after waiting to see if any more changes are being made
void Config::ScheduleSave()
{
	scheduleSave = true;
	saveBooked = SysTickVal;
}


void Config::RequestSave()
{
	scheduleSave = true;
	saveNow = true;
}


void Config::CheckAutoSave()
{
	// Main loop: save once the settings have been left alone for a moment
	if (!scheduleSave) {
		return;
	}
	const uint32_t now = SysTickVal;
	if (saveNow || (now - saveBooked > autoSaveDelayMs && now - lastSave > autoSaveMinGapMs)) {
		SaveConfig();
	}
}


// Write settings to Flash memory (H743 see programming manual p152 for sector layout)
// modulove: runs in the background from the main loop. The config sector is in bank 2 and the code runs from bank 1,
// so the audio interrupt keeps running while bank 2 is programmed or erased (read-while-write between banks).
// Nothing reads bank 2 during normal operation (the config is copied to RAM at boot).
bool Config::SaveConfig(bool eraseOnly)
{
	scheduleSave = false;
	saveNow = false;
	lastSave = SysTickVal;
	bool result = true;

	uint32_t latest = sectorSize;
	const uint32_t freePos = eraseOnly ? sectorSize : FindRecords(latest);
	const uint32_t cfgSize = eraseOnly ? 0 : SetConfig();
	const uint32_t needed = (cfgSize + 31) & ~31;			// programmed in 32 byte flash words

	FlashUnlock(2);											// Unlock Flash memory for writing
	// modulove: error flags are cleared through CCR2 (writing SR2 does nothing). Upstream wrote SR2, so one error
	// (seen 2026-11-16: PGSERR + INCERR) made every later save silently write nothing until the next reset.
	FLASH->CCR2 = FLASH_ALL_ERRORS;

	if (eraseOnly) {
		result = WriteRecord(0, 0, true);
	} else {
		result = WriteRecord(freePos, cfgSize, freePos + needed > sectorSize);	// sector full (or unknown data): erase
		if (!result) {
			++failedSaves;
			FLASH->CCR2 = FLASH_ALL_ERRORS;
			result = WriteRecord(0, cfgSize, true);			// retry once on a freshly erased sector
			if (!result) {
				++failedSaves;
			}
		}
		++saveCount;
	}

	FlashLock(2);											// Lock Bank 2 Flash
	return result;
}


bool Config::WriteRecord(uint32_t freePos, const uint32_t cfgSize, const bool erase)
{
	if (erase) {
		FlashEraseSector(7, 2);								// Erase sector 7, Bank 2 (See p152 of manual)
		const bool ok = FlashWaitForLastOperation(2);
		FLASH->CR2 &= ~(FLASH_CR_SER | FLASH_CR_SNB);		// modulove: end the erase request (as ST's HAL does)
		SCB_InvalidateDCache_by_Addr(addrFlashBank2Sector7, sectorSize);
		++eraseCount;
		freePos = 0;
		if (!ok) {
			return false;
		}
	}
	if (cfgSize == 0) {
		return true;
	}
	const uint32_t needed = (cfgSize + 31) & ~31;
	const bool programmed = FlashProgram(addrFlashBank2Sector7 + freePos / 4, reinterpret_cast<uint32_t*>(&configBuffer), cfgSize);
	SCB_InvalidateDCache_by_Addr(addrFlashBank2Sector7 + freePos / 4, needed);
	// modulove: verify - the new record must be complete and identical (boot loads the newest complete record)
	const uint8_t* rec = reinterpret_cast<const uint8_t*>(addrFlashBank2Sector7) + freePos;
	return programmed && RecordLength(rec, sectorSize - freePos) == cfgSize && memcmp(rec, configBuffer, cfgSize) == 0;
}


uint32_t Config::RecordLength(const uint8_t* rec, const uint32_t maxLen)
{
	// Length of a complete record ("CFG", version, blocks, "END"), 0 if invalid or incomplete
	if (maxLen < 12 || strncmp(reinterpret_cast<const char*>(rec), "CFG", 4) != 0 || rec[4] != configVersion) {
		return 0;
	}
	uint32_t pos = 8;
	while (pos + 4 <= maxLen && pos + 4 <= BufferSize) {
		if (strncmp(reinterpret_cast<const char*>(&rec[pos]), "END", 4) == 0) {
			return pos + 4;
		}
		const uint16_t id  = *reinterpret_cast<const uint16_t*>(&rec[pos]);
		const uint16_t len = *reinterpret_cast<const uint16_t*>(&rec[pos + 2]);
		if (id == 0xFFFF || pos + 4 + len > maxLen) {
			return 0;
		}
		pos += 4 + ((len + 3) & ~3);
	}
	return 0;
}


uint32_t Config::FindRecords(uint32_t& latest)
{
	// Walk the appended records: the newest complete one wins; an interrupted save is ignored (the one before is used)
	SCB_InvalidateDCache_by_Addr(addrFlashBank2Sector7, sectorSize);
	const uint8_t* base = reinterpret_cast<const uint8_t*>(addrFlashBank2Sector7);
	latest = sectorSize;
	uint32_t pos = 0;
	while (pos + 32 <= sectorSize) {
		if (*reinterpret_cast<const uint32_t*>(base + pos) == 0xFFFFFFFF) {
			return pos;										// erased: free from here
		}
		const uint32_t len = RecordLength(base + pos, sectorSize - pos);
		if (len == 0) {
			return sectorSize;								// unknown or incomplete data: erase on the next save
		}
		latest = pos;
		pos += (len + 31) & ~31;
	}
	return pos;
}


uint32_t Config::AddBlock(uint32_t pos, const uint16_t id, const uint8_t* data, const uint32_t len)
{
	// Block layout: uint16 id, uint16 length, data padded to 4 bytes
	if (pos + 4 + len + 8 > BufferSize) {
		return pos;													// No room (leave space for footer)
	}
	*reinterpret_cast<uint16_t*>(&configBuffer[pos]) = id;
	*reinterpret_cast<uint16_t*>(&configBuffer[pos + 2]) = static_cast<uint16_t>(len);
	memcpy(&configBuffer[pos + 4], data, len);
	return pos + 4 + ((len + 3) & ~3);
}


uint32_t Config::SetConfig()
{
	// Serialise config values into buffer
	memset(configBuffer, 0, sizeof(configBuffer));					// Clear buffer
	strncpy(reinterpret_cast<char*>(configBuffer), "CFG", 4);		// Header
	configBuffer[4] = configVersion;
	uint32_t configPos = 8;											// Position in buffer to store data

	uint8_t* cfgBuffer = nullptr;

	// Voice configurations (block id = base + voice number)
	for (auto& nm : voiceManager.noteMapper) {
		if (nm.drumVoice != nullptr && nm.voiceIndex == 0) {		// If voiceIndex is > 0 drum voice has multiple channels (eg sampler)
			const uint32_t size = nm.drumVoice->SerialiseConfig(&cfgBuffer, nm.voiceIndex);
			if (size > 0) {
				configPos = AddBlock(configPos, static_cast<uint16_t>(blockVoiceBase) + nm.voice, cfgBuffer, size);
			}
		}
	}

	uint32_t size = reverb.SerialiseConfig(&cfgBuffer);
	configPos = AddBlock(configPos, blockReverb, cfgBuffer, size);

	size = voiceManager.GetConfig(&cfgBuffer);
	configPos = AddBlock(configPos, blockMidiMap, cfgBuffer, size);

	size = voiceManager.GetMixConfig(&cfgBuffer);
	configPos = AddBlock(configPos, blockMix, cfgBuffer, size);

	size = sequencer.GetSequences(&cfgBuffer);
	configPos = AddBlock(configPos, blockSequences, cfgBuffer, size);

	// Footer
	strncpy(reinterpret_cast<char*>(&configBuffer[configPos]), "END", 4);
	configPos += 4;
	return configPos;
}


// Restore configuration settings from flash memory
void Config::RestoreConfig()
{
	// Initialise derived values from defaults first - stored blocks then override (shorter/older blocks keep new defaults)
	for (auto& nm : voiceManager.noteMapper) {
		if (nm.drumVoice != nullptr && nm.voiceIndex == 0) {
			nm.drumVoice->StoreConfig(nullptr, 0);
		}
	}
	reverb.StoreConfig(nullptr);

	uint8_t* reverbCfg = nullptr;
	const uint32_t reverbSize = reverb.SerialiseConfig(&reverbCfg);

	// Newest complete record in the sector. Older versions (incl. upstream v5/v6) are not recognised: defaults until saved
	uint32_t latest;
	FindRecords(latest);
	if (latest >= sectorSize) {
		return;
	}
	const uint8_t* flashConfig = reinterpret_cast<uint8_t*>(addrFlashBank2Sector7) + latest;

	uint32_t pos = 8;
	while (pos + 4 <= BufferSize) {
		if (strncmp((const char*)&flashConfig[pos], "END", 4) == 0) {
			break;
		}
		const uint16_t id  = *reinterpret_cast<const uint16_t*>(&flashConfig[pos]);
		const uint16_t len = *reinterpret_cast<const uint16_t*>(&flashConfig[pos + 2]);
		uint8_t* data = const_cast<uint8_t*>(&flashConfig[pos + 4]);
		if (id == 0xFFFF || pos + 4 + len > BufferSize) {			// Erased flash or corrupt length
			break;
		}

		if (id >= blockVoiceBase && id < static_cast<uint16_t>(blockVoiceBase) + VoiceManager::Voice::count) {
			auto& nm = voiceManager.noteMapper[id - blockVoiceBase];
			if (nm.drumVoice != nullptr && nm.voiceIndex == 0) {
				nm.drumVoice->StoreConfig(data, std::min((uint32_t)len, nm.drumVoice->ConfigSize()));
			}
		} else if (id == blockReverb && len >= reverbSize) {
			reverb.StoreConfig(data);
		} else if (id == blockMidiMap) {
			voiceManager.StoreConfig(data, len);
		} else if (id == blockMix) {
			voiceManager.StoreMixConfig(data, len);
		} else if (id == blockSequences && len == sequencer.SequencesSize()) {
			sequencer.StoreSequences(data);
		}

		pos += 4 + ((len + 3) & ~3);
	}
}



// Unlock the FLASH control register access
void Config::FlashUnlock(uint8_t bank)
{
	volatile uint32_t* bankCR  = &(bank == 1 ? FLASH->CR1 : FLASH->CR2);
	volatile uint32_t* bankKEY = &(bank == 1 ? FLASH->KEYR1 : FLASH->KEYR2);

	if ((*bankCR & FLASH_CR_LOCK) != 0)  {
		*bankKEY = 0x45670123U;					// These magic numbers unlock the bank for programming
		*bankKEY = 0xCDEF89ABU;
	}
}


// Lock the FLASH Bank Registers access
void Config::FlashLock(uint8_t bank)
{
	if (bank == 1) {
		FLASH->CR1 |= FLASH_CR_LOCK;
	} else {
		FLASH->CR2 |= FLASH_CR_LOCK;
	}
}


// Erase sector - FLASH_CR_PSIZE_1 means a write size of 32bits (see p163 of manual)
void Config::FlashEraseSector(uint8_t Sector, uint32_t bank)
{
	volatile uint32_t* bankCR = &(bank == 1 ? FLASH->CR1 : FLASH->CR2);

	*bankCR &= ~(FLASH_CR_PSIZE | FLASH_CR_SNB);
	*bankCR |= (FLASH_CR_SER |							// Sector erase request
			FLASH_CR_PSIZE_1 |							// Write 32 bits at a time
			(Sector << FLASH_CR_SNB_Pos) |
			FLASH_CR_START);
}


bool Config::FlashWaitForLastOperation(uint32_t bank)
{
    // Even if FLASH operation fails, the QW flag will be reset and an error flag will be set
	volatile uint32_t* bankSR  = &(bank == 1 ? FLASH->SR1 : FLASH->SR2);
	volatile uint32_t* bankCCR = &(bank == 1 ? FLASH->CCR1 : FLASH->CCR2);

	if (*bankSR & FLASH_ALL_ERRORS) {					// If any error occurred abort
		lastFlashError = *bankSR & FLASH_ALL_ERRORS;
		*bankCCR = FLASH_ALL_ERRORS;					// Clear all errors (modulove: CCR - upstream wrote SR, which is read only)
		return false;
	}

	while ((*bankSR & FLASH_SR_QW) == FLASH_SR_QW) {	// QW flag set when write or erase operation is pending in the command queue buffer

	}

	if ((*bankSR & FLASH_SR_EOP) == FLASH_SR_EOP) {		// Check End of Operation flag
		*bankCCR = FLASH_CCR_CLR_EOP;
	}

	return true;
}


bool Config::FlashProgram(uint32_t* dest_addr, uint32_t* src_addr, size_t size)
{
	uint8_t bank = (reinterpret_cast<uintptr_t>(dest_addr) < FLASH_BANK2_BASE) ? 1 : 2;		// Get which bank we are programming from destination address
	volatile uint32_t* bankCR = &(bank == 1 ? FLASH->CR1 : FLASH->CR2);

	if (Config::FlashWaitForLastOperation(bank)) {
		*bankCR |= FLASH_CR_PG;

		__ISB();
		__DSB();

		// Each write block is 32 bytes
		for (uint16_t b = 0; b < std::ceil(static_cast<float>(size) / 32); ++b) {
			for (uint8_t i = 0; i < FLASH_NB_32BITWORD_IN_FLASHWORD; ++i) {
				*dest_addr = *src_addr;
				dest_addr++;
				src_addr++;
			}

			if (!Config::FlashWaitForLastOperation(bank)) {
				*bankCR &= ~FLASH_CR_PG;				// Clear programming flag
				return false;
			}
		}

		__ISB();
		__DSB();

		*bankCR &= ~FLASH_CR_PG;						// Clear programming flag
		return true;
	}
	return false;										// modulove: nothing was programmed (upstream returned true)
}


