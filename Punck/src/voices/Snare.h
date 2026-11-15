#pragma once
#include "initialisation.h"
#include "DrumVoice.h"
#include "Filter.h"
#include "plaits/dsp/drums/analog_snare_drum.h"
#include "plaits/dsp/drums/synthetic_snare_drum.h"
#include "plaits/dsp/physical_modelling/modal_drum.h"

class NoteMapper;


class Snare : public DrumVoice {
public:
	enum Engine : uint8_t {classic = 0, analog, synthetic, rim, modal, engineCount};

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
	// Idle skip: engines that render continuously stop when silent and restart on the next hit
	static constexpr uint32_t quietLimit = 4800;		// 100 ms below -94 dBFS
	uint32_t quietCount = 0;
	bool idle = false;
	plaits::AnalogSnareDrum analogSd;
	plaits::SyntheticSnareDrum syntheticSd;

	// Rim engine: two decaying sines + noise click
	float rimPhase[2] = {}, rimInc[2] = {}, rimEnv[2] = {}, rimCoef[2] = {};
	float clickEnv = 0.0f, clickCoef = 0.0f;

	// Modal engine: Mutable Instruments modal resonator (Rings / Plaits) struck by a mallet + snare wire noise
	plaits::ModalDrum modalDrum;
	stmlib::Svf wireBp;
	float wireEnv = 0.0f, wireCoef = 0.99f;
	bool modalReady = false;
	static constexpr float modalGain = 0.45f;		// resonator output scaling: matches the 808 snare level (tools/hostsim)

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

		// Modal engine (Filter pot = mallet brightness, Tuning = pitch, Decay = damping)
		float modalStructure = 0.15f;			// 0-0.25 drum (compressed partials), 0.3 harmonic, up to 1 bell / plate
		float modalWires = 0.5f;				// Snare wire noise level
		float modalWireTone = 4500.0f;			// Snare wire band (Hz)
		float modalWireDecay = 140.0f;			// Snare wire decay (ms)
		float modalModes = 16.0f;				// Resonator modes 4 - 24 (more = brighter, more CPU)
		float modalLevel = 1.0f;				// Output trim
	} config;

	Filter<2> filter{filterPass::LowPass, &(ADC_array[ADC_SnareFilter])};		// Classic: filters partial and noise elements
};
