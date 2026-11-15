#pragma once
// Host (PC) stand-in for Punck/src/voices/VoiceManager.h: only what the voices use (LED, tempo, sidechain)
#include "initialisation.h"
#include "Kick.h"
#include "Snare.h"
#include "HiHat.h"

struct NoteMapper {
	struct PWMLED { void Level(float) {} } pwmLed;
};

class VoiceManager {
public:
	float duckLevel = 0.0f;
	float sixteenth = 0.0f;							// samples per 16th (0 = unknown tempo)
	float duckHoldMs = 25.0f, duckReleaseMs = 160.0f;

	float SixteenthSamples() { return sixteenth; }
	void KickTriggered(const float velocity) {		// same envelope as the firmware
		duckTarget = std::max(duckTarget, std::clamp(velocity, 0.0f, 1.0f));
		duckHold = static_cast<uint32_t>(duckHoldMs * 0.001f * systemSampleRate);
	}
	void UpdateDuck() {
		if (duckHold > 0) {
			--duckHold;
		} else {
			duckTarget *= std::exp(-1.0f / (duckReleaseMs * 0.001f * systemSampleRate));
		}
		duckLevel += (duckTarget - duckLevel) * 0.02f;
	}
private:
	float duckTarget = 0.0f;
	uint32_t duckHold = 0;
};
extern VoiceManager voiceManager;
