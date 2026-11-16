#pragma once

#include "initialisation.h"

// Abstract Class to define interface for drum voices

class DrumVoice {
public:
	float outputLevel[2];
	bool playing;
	uint32_t debugMaxTime = 0;
	uint32_t debugTime = 0;

	// pitch is a frequency ratio (1.0 = voice's configured pitch); used by chromatic MIDI channels
	virtual void Play(uint8_t voice, uint32_t noteOffset, uint32_t noteRange, float velocity, float pitch) = 0;
	virtual void Play(const uint8_t voice, const uint32_t index) = 0;
	virtual void CalcOutput() = 0;
	virtual uint32_t SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex) = 0;		// Return a pointer to config data for saving or transmission over SysEx
	virtual void StoreConfig(uint8_t* buff, const uint32_t len) = 0;					// Reads config data back into member values
	virtual uint32_t ConfigSize() = 0;
	virtual void UpdateFilter() {};

	// Switchable sound engines (long press on the voice button cycles through them)
	virtual uint8_t EngineCount() { return 1; }
	virtual uint8_t GetEngine() { return 0; }
	virtual void SetEngine(const uint8_t e) {}

	// One queue slot per player: the sampler voice drives two players (A/B) which may be triggered simultaneously
	static constexpr uint8_t queueSlots = 2;
	volatile bool noteQueued[queueSlots] = {};	// Declared volatile to ensure that bool is only set once values are stored ready for playing
	struct QueuedNote {
		uint8_t voice;
		uint32_t noteOffset;
		uint32_t noteRange;
		float velocity;
		float pitch;
	} queuedNote[queueSlots];

	// When note is triggered outside main interrupt queue to play in main interrupt
	void QueuePlay(const uint8_t voice, const uint32_t offset, const uint32_t range, const float velocity, const float pitch = 1.0f) {
		const uint8_t slot = voice % queueSlots;
		noteQueued[slot] = false;
		queuedNote[slot].voice = voice;
		queuedNote[slot].noteOffset = offset;
		queuedNote[slot].noteRange = range;
		queuedNote[slot].velocity = velocity;
		queuedNote[slot].pitch = pitch;
		noteQueued[slot] = true;
	}

	// Audio interrupt (VoiceManager::StartOneHit): plays the note queued in one slot (slot = voice index), if any
	bool PlayQueuedSlot(const uint8_t slot) {
		if (slot >= queueSlots || !noteQueued[slot]) {
			return false;
		}
		Play(queuedNote[slot].voice, queuedNote[slot].noteOffset, queuedNote[slot].noteRange, queuedNote[slot].velocity, queuedNote[slot].pitch);
		noteQueued[slot] = false;
		return true;
	}

	// Called in main interrupt to check if queued note should be played
	void PlayQueued() {
		for (uint8_t s = 0; s < queueSlots; ++s) {
			if (noteQueued[s]) {
				Play(queuedNote[s].voice, queuedNote[s].noteOffset, queuedNote[s].noteRange, queuedNote[s].velocity, queuedNote[s].pitch);
				noteQueued[s] = false;
			}
		}
	}

	// Random zone at the top of a pot's travel (kick and hat Decay): mostly normal hits around 'base' with a little
	// variation, plus a few sustained ones - their chance rises from longMin where the zone starts to longMax at full
	// CW. Below the zone the pot is rescaled so its whole range is still available. r1, r2: random 0 - 1.
	static float RandomZone(const float pot, float zone, const float longMin, const float longMax, const float base,
			const float r1, const float r2)
	{
		zone = std::clamp(zone, 0.0f, 0.5f);
		if (zone <= 0.0f) {
			return pot;
		}
		const float start = 1.0f - zone;
		if (pot < start) {
			return pot / start;
		}
		const float z = std::min((pot - start) / zone, 1.0f);
		const float chance = longMin + (longMax - longMin) * z;
		return (r1 < chance) ? 0.8f + 0.2f * r2 : std::clamp(base + 0.2f * (r2 - 0.5f), 0.0f, 1.0f);
	}

	constexpr float FreqToInc(const float frequency)
	{
		return frequency * (2 * pi) / systemSampleRate;
	}
};

