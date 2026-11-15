#pragma once
#include "initialisation.h"
#include "Filter.h"
#include "DrumVoice.h"
#include "plaits/dsp/drums/analog_bass_drum.h"
#include "plaits/dsp/drums/synthetic_bass_drum.h"
#include "stmlib/dsp/filter.h"

class NoteMapper;

// Rumble processor (modulove, own design inspired by the classic 'kick -> reverb -> drive -> low pass ->
// sidechain' techno rumble): a small 4-line feedback delay network running at 1/8 sample rate (6 kHz is
// plenty for content below ~300 Hz), saturation and a 2-pole low pass, upsampled with a one-pole smoother.
class Rumble {
public:
	void Init();
	float Process(const float in, const float feedback, const float cutoffHz, const float drive);
private:
	static constexpr uint32_t decimation = 8;
	static constexpr uint32_t lineLen[4] = {211, 263, 331, 409};		// samples at 6 kHz (35 - 68 ms)
	float line[4][409];
	uint32_t pos[4];
	uint32_t phase = 0;
	float preLp = 0.0f;					// anti-alias low pass before decimation
	float held = 0.0f, smooth = 0.0f;
	stmlib::Svf lp;
};


class Kick : public DrumVoice {
public:
	enum Engine : uint8_t {classic = 0, deep, punch, fm, rumble, engineCount};

	void Play(const uint8_t voice, const uint32_t noteOffset, const uint32_t noteRange, const float velocity, const float pitch);
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
	float CalcFm();

	// Classic (upstream) engine state
	enum class Phase {Off, Ramp1, Ramp2, Ramp3, FastSine, SlowSine} phase;
	Filter<2> filter{filterPass::LowPass, &(ADC_array[ADC_KickAttack])};
	float position;
	float currentLevel;
	float slowSinInc;
	float slowSinLevel;
	float fastSinInc = FreqToInc(130.0f);

	// Shared
	Engine engine = deep;
	float velocityScale;
	float notePitch = 1.0f;				// Pitch ratio of current note (chromatic MIDI channel)
	float accent = 0.8f;
	bool trigger = false;				// Pending trigger for the sample based engines
	float knobA = 0.5f, knobB = 0.5f;	// Smoothed Attack / Decay pots (0 - 1)
	float ledEnv = 0.0f;

	// Plaits engines (Mutable Instruments, MIT)
	plaits::AnalogBassDrum analogBd;
	plaits::SyntheticBassDrum syntheticBd;
	plaits::AnalogBassDrum ghostBd;		// Ghost taps for the rumble engine (fed to the rumble only)

	// FM engine state
	float fmPhaseC = 0.0f, fmPhaseM = 0.0f, fmAmp = 0.0f, fmPitchEnv = 0.0f, fmIndexEnv = 0.0f;

	// Rumble engine state
	Rumble rumbleFx;
	int32_t tapCountdown[3] = {-1, -1, -1};
	bool ghostTrigger = false;
	float ghostAccent = 0.0f;

	struct Config {
		// Classic engine (upstream layout - keep order for the web editor)
		float ramp1Inc = 0.22f;					// Initial steep ramp
		float ramp2Inc = 0.015f;				// Second slower ramp
		float ramp3Inc = 0.3f;					// Steep Discontinuity leading to fast sine
		float fastSinFreq = 130.0f;				// Higher frequency of fast sine section
		float initSlowSinFreq = 60.0f;			// Initial frequency of gradually decreasing slow sine wave
		float sineSlowDownRate = 0.999985f;		// Rate at which slow sine wave frequency decreases

		// modulove engines
		float engine = 1.0f;					// 0 classic, 1 deep, 2 punch, 3 FM, 4 rumble
		float pitch = 49.0f;					// Base pitch (Hz) of engines 1-4 (G1)
		float tone = 0.45f;						// Plaits 'timbre': brightness of the attack
		float fmRatio = 1.41f;					// FM: modulator / carrier frequency ratio
		float fmPitchEnv = 3.0f;				// FM: pitch at hit = pitch x (1 + this)
		float fmPitchDecay = 22.0f;				// FM: pitch envelope time constant (ms)
		float drive = 0.25f;					// FM + rumble body: output saturation 0 - 1
		float rumbleFeedback = 0.86f;			// Rumble: reverb feedback (tail length) 0 - 0.97
		float rumbleCutoff = 140.0f;			// Rumble: low pass (Hz)
		float rumbleDrive = 0.6f;				// Rumble: saturation 0 - 1
		float rumbleDuck = 0.9f;				// Rumble: ducked by each kick 0 - 1
		float rumbleLevel = 1.4f;				// Rumble: output level at full Attack knob
		float rumblePunch = 0.3f;				// Rumble body: Plaits harmonics (Attack knob is the rumble amount)
		float tap2 = 0.6f;						// Rumble ghost taps on the 1st, 2nd, 3rd 16th after each kick
		float tap3 = 0.4f;
		float tap4 = 0.25f;
	} config;
};
