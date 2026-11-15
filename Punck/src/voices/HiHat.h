#pragma once
#include "initialisation.h"
#include "Filter.h"
#include "BPFilter.h"
#include "DrumVoice.h"
#include "plaits/dsp/drums/hi_hat.h"

class NoteMapper;
class HiHat  : public DrumVoice {
public:
	enum Engine : uint8_t {classic = 0, square808, ringMod909, engineCount};

	void Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch);
	void Play(const uint8_t voice, const uint32_t index);
	void CalcOutput();
	void UpdateFilter() override;
	uint32_t SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex);
	void StoreConfig(uint8_t* buff, const uint32_t len);
	uint32_t ConfigSize();
	uint8_t EngineCount() override { return engineCount; }
	uint8_t GetEngine() override { return engine; }
	void SetEngine(const uint8_t e) override;

	NoteMapper* noteMapper;

private:
	void CalcClassic();

	// Plaits hi-hats (Mutable Instruments, MIT): 808-ish square noise and 909-ish ring modulated noise
	Engine engine = square808;
	plaits::HiHat<plaits::SquareNoise, plaits::SwingVCA, true, false> hat808;
	plaits::HiHat<plaits::RingModNoise, plaits::LinearVCA, false, true> hat909;
	bool trigger = false;
	float accent = 0.8f;
	float f0 = 0.008f;
	float morph = 0.3f;						// Plaits decay for the current hit
	float ledEnv = 0.0f;
	// Idle skip: engines that render continuously stop when silent and restart on the next hit
	static constexpr uint32_t quietLimit = 4800;		// 100 ms below -94 dBFS
	uint32_t quietCount = 0;
	bool idle = false;

	// Float fields first: the web editor edits the config as a flat list of floats (new fields appended)
	struct Config {
		float attackInc = 0.005f;			// Attack envelope speed
		float decay = 0.9991f;				// (unused - decay is set from pot and note)

		float hpInitCutoff = 0.07f;			// HP filter ramps from 1.68kHz to ~7.9kHz (closed hats crisper than upstream 6.48kHz)
		float hpFinalCutoff = 0.33f;
		float hpCutoffInc = 1.01f;

		float lpInitCutoff = 0.75f;			// LP starts at 18kHz (upstream 12kHz) ...
		float lpFinalCutoff = 0.1f;
		float lpCutoffInc = 0.999993f;		// ... and closes ~3x slower so open hats stay airy

		float noiseInitLevel = 1.3f;		// Initial level of noise component (upstream 0.99)
		float noiseDecay = 0.9998f;

		// Relative level and frequency of 6 signal partials
		float partialScale[6] = {0.8f, 0.5f, 0.4f, 0.4f, 0.5f, 0.4f};
		float partialFreq[6] = {569.0f, 621.0f, 1559.0f, 2056.0f, 3300.0f, 5515.0f};

		// --- modulove additions ---
		float decayMin = 0.9968f;			// Per-sample decay multiplier when fully closed (~45 ms to -60 dB)
		float decayRange = 0.0030f;			// Added per unit of 'open' amount (note offset + decay pot)
		float decayMax = 0.99974f;			// Longest decay (fully open hat ~0.55 s to -60 dB)
		float ringModLevel = 0.6f;			// Level of ring modulated metallic layer on fully open hats
		float ringModOpenStart = 0.35f;		// Open amount at which ring mod layer starts to fade in
		float gritHold = 3.0f;				// Sample & hold length of ring mod layer (1 = clean, higher = grittier)
		float randomThreshold = 0.96f;		// (unused since the random zone below - kept for the config layout)
		float randomSkew = 2.0f;			// (unused)

		// Frequency modulation - amount by which partial 0 frequency modulates other partials
		uint8_t partialFM[6] = {0, 8, 1, 6, 1, 0};

		// Engines (after partialFM so blocks saved by earlier modulove firmware still load correctly)
		float engine = 1.0f;				// 0 classic metallic, 1 808-style square noise, 2 909-style ring mod noise
		float pitch = 400.0f;				// Base frequency (Hz) of the Plaits hats' metallic oscillators
		float tone = 0.6f;					// Plaits 'timbre' (filter brightness)
		float noisiness = 0.35f;			// Plaits 'harmonics': metallic -> noisy

		// Random zone at the top of the Decay pot: mostly short hits, a few sustained ones - more towards full CW
		float randomZone = 0.10f;			// Fraction of the pot travel (0 = off)
		float randomLongMin = 0.08f;		// Chance of a sustained hit where the zone starts ...
		float randomLongMax = 0.6f;			// ... and at full CW
		float randomBase = 0.25f;			// Decay of the normal hits in the zone (0 - 1 like the pot)

		// 808 / 909 engines: open amount -> Plaits decay (shorter overall than the first version: 0.25 + 0.5 x open)
		float plaitsDecayBase = 0.05f;
		float plaitsDecayScale = 0.45f;
		float plaitsDecayMax = 0.7f;
	} config;

	Filter<2> hpFilter{filterPass::HighPass, nullptr};
	Filter<2> lpFilter{filterPass::LowPass, nullptr};

	float velocityScale;
	float decayScale;
	bool attack;							// True during the envelope attack phase
	float attackLevel;
	float noiseScale;

	float hpFilterCutoff;
	float lpFilterCutoff;

	float ringLevel = 0.0f;					// Ring mod layer level for current note
	uint32_t gritCounter = 0;
	float gritSample = 0.0f;

	// multiplier to convert frequency to half a period of a square wave
	static constexpr uint32_t freqToSqPeriod = systemSampleRate / 2;

	float partialLevel[6];					// Output level of each partial
	uint32_t partialPeriod[6];				// Square wave period
	uint32_t partialPos[6];					// partial position counter

};
