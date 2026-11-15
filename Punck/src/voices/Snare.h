#pragma once
#include "initialisation.h"
#include "DrumVoice.h"
#include "Filter.h"
#include "plaits/dsp/drums/analog_snare_drum.h"
#include "plaits/dsp/drums/synthetic_snare_drum.h"

class NoteMapper;


class Snare : public DrumVoice {
public:
	enum Engine : uint8_t {classic = 0, analog, synthetic, rim, engineCount};

	void Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch);
	void Play(const uint8_t voice, const uint32_t index);
	void CalcOutput();
	void UpdateFilter();
	uint32_t SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex);
	void StoreConfig(uint8_t* buff, const uint32_t len);
	uint32_t ConfigSize();
	uint8_t EngineCount() override { return engineCount; }
	uint8_t GetEngine() override { return engine; }
	void SetEngine(const uint8_t e) override;

	NoteMapper* noteMapper;
private:
	void CalcClassic();

	// Classic (upstream) engine state
	static constexpr uint8_t partialCount = 3;
	float partialLevel[partialCount];
	float partialpos[partialCount];
	float partialInc[partialCount];
	float noiseLevel;
	float velocityScale;
	float sustainRate;

	// Engine state
	Engine engine = analog;
	bool trigger = false;
	float accent = 0.8f;
	float f0 = 0.004f;					// Normalised frequency for the current hit
	float snappy = 0.5f;
	float decayEff = 0.4f;
	float ledEnv = 0.0f;
	plaits::AnalogSnareDrum analogSd;
	plaits::SyntheticSnareDrum syntheticSd;

	// Rim engine: two decaying sines + noise click
	float rimPhase[2] = {}, rimInc[2] = {}, rimEnv[2] = {}, rimCoef[2] = {};
	float clickEnv = 0.0f, clickCoef = 0.0f;

	struct Config {
		// Classic engine (upstream layout - keep order for the web editor)
		float noiseInitLevel = 1.0f;
		float noiseDecay = 0.9982f;				// Decay rate of noise (increased by decay amount in MIDI mode)
		float baseFreq = 150.0f;				// Lowest frequency which partials will be non-integer multiples of
		float basePos = 0.5f;					// Starting position of initial partial in radians
		float partialDecay = 0.9982f;			// Decay rate of partials (increased by decay amount in MIDI mode)
		float partialInitLevel[partialCount] = {0.7f, 0.5f, -0.4};
		float partialFreqOffset[partialCount] = {1.0f, 1.588f, 1.833f};

		// modulove engines
		float engine = 1.0f;					// 0 classic, 1 analog (808), 2 synthetic (909), 3 rim/clave
		float pitch = 200.0f;					// Pitch (Hz) at Tuning knob centre; knob = x0.5 .. x2
		float tone = 0.5f;						// Plaits 'timbre' (analog: tone, synthetic: FM amount)
		float decay = 0.35f;					// Plaits decay 0 - 1 (MIDI note offset / open trigger adds up to +0.4)
		float rimDecay = 45.0f;					// Rim: decay time (ms)
		float rimRatio = 2.17f;					// Rim: second partial ratio
	} config;

	Filter<2> filter{filterPass::LowPass, &(ADC_array[ADC_SnareFilter])};		// Classic: filters partial and noise elements
};
