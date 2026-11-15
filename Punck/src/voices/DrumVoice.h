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

	// Called in main interrupt to check if queued note should be played
	void PlayQueued() {
		for (uint8_t s = 0; s < queueSlots; ++s) {
			if (noteQueued[s]) {
				Play(queuedNote[s].voice, queuedNote[s].noteOffset, queuedNote[s].noteRange, queuedNote[s].velocity, queuedNote[s].pitch);
				noteQueued[s] = false;
			}
		}
	}

	constexpr float FreqToInc(const float frequency)
	{
		return frequency * (2 * pi) / systemSampleRate;
	}
};

