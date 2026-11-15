#pragma once
#include "initialisation.h"
#include "Filter.h"
#include "BPFilter.h"
#include "DrumVoice.h"

class NoteMapper;
class HiHat  : public DrumVoice {
public:
	void Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch);
	void Play(const uint8_t voice, const uint32_t index);
	void CalcOutput();
	void UpdateFilter() override;
	uint32_t SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex);
	void StoreConfig(uint8_t* buff, const uint32_t len);
	uint32_t ConfigSize();

	NoteMapper* noteMapper;

private:
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
		float decayMin = 0.9980f;			// Per-sample decay multiplier when fully closed
		float decayRange = 0.0028f;			// Added per unit of 'open' amount (note offset + decay pot)
		float decayMax = 0.99985f;			// Longest decay (fully open hat rings ~1s)
		float ringModLevel = 0.6f;			// Level of ring modulated metallic layer on fully open hats
		float ringModOpenStart = 0.35f;		// Open amount at which ring mod layer starts to fade in
		float gritHold = 3.0f;				// Sample & hold length of ring mod layer (1 = clean, higher = grittier)
		float randomThreshold = 0.96f;		// Decay pot position above which decay is randomised per hit
		float randomSkew = 2.0f;			// Random decay distribution: 1 = uniform, >1 = mostly closed with occasional open hits

		// Frequency modulation - amount by which partial 0 frequency modulates other partials
		uint8_t partialFM[6] = {0, 8, 1, 6, 1, 0};
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
