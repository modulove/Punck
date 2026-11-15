#pragma once
// Host (PC) stand-in for Punck/src/voices/VoiceManager.h: only what the voices use (LED, tempo, sidechain)
#include "initialisation.h"
#include "Kick.h"
#include "Snare.h"
#include "HiHat.h"
#include "Toms.h"
#include "Dynamics.h"
#include "Harmony.h"

struct NoteMapper {
	struct PWMLED { void Level(float) {} } pwmLed;
};

class VoiceManager {
public:
	enum Voice {kick, snare, hihat, samplerA, samplerB, toms, claps, count};
	int32_t keyRoot = -1;									// same Key & scale logic as the firmware
	uint16_t tuneMask[count] = {};
	void SetKey(int32_t root, uint32_t scale, const uint32_t modes[count]) {
		keyRoot = root;
		for (uint32_t v = 0; v < count; ++v) tuneMask[v] = harmony::ModeMask(modes[v], scale);
	}
	float Tune(const Voice v, const float hz) const {
		if (keyRoot < 0 || hz <= 0.0f || tuneMask[v] == 0) return hz;
		const float note = 69.0f + 12.0f * std::log2(hz / 440.0f);
		return 440.0f * dyn::FastExp2((harmony::QuantizeNote(note, keyRoot, tuneMask[v]) - 69.0f) / 12.0f);
	}
	bool TuneActive(const Voice v) const { return keyRoot >= 0 && tuneMask[v] != 0; }
	float duckLevel = 0.0f;
	float sixteenth = 0.0f;							// samples per 16th (0 = unknown tempo)
	float duckHoldMs = 25.0f, duckReleaseMs = 160.0f;
	Toms* bassVoice = nullptr;						// gets the auto bass notes after each kick

	float SixteenthSamples() { return sixteenth; }
	void KickTriggered(const float velocity) {		// same envelope as the firmware
		duckTarget = std::max(duckTarget, std::clamp(velocity, 0.0f, 1.0f));
		duckHold = static_cast<uint32_t>(duckHoldMs * 0.001f * systemSampleRate);
		if (bassVoice) bassVoice->KickPlayed(sixteenth, velocity);
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
