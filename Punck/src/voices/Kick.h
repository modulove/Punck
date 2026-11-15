#pragma once
#include "initialisation.h"
#include "Filter.h"
#include "DrumVoice.h"
#include "plaits/dsp/drums/analog_bass_drum.h"
#include "plaits/dsp/drums/synthetic_bass_drum.h"
#include "stmlib/dsp/filter.h"

class NoteMapper;

// Rumble processor (modulove, own design inspired by the classic 'kick -> drive -> reverb -> drive -> low pass ->
// sidechain' techno rumble): a small 4-line feedback delay network running at 1/8 sample rate (6 kHz is
// plenty for content below ~300 Hz), saturation, 2-pole low pass and high pass, upsampled with a one-pole smoother.
// Stereo: mid = sum of all lines (identical to a mono rumble, so a mono PA loses nothing), side = orthogonal mix.
class Rumble {
public:
	void Init();
	void Configure(const float feedback, const float cutoffHz, const float drive, const float hpHz, const float width);
	void Process(const float in, float& outL, float& outR);
private:
	static constexpr uint32_t decimation = 8;
	static constexpr float rate = static_cast<float>(systemSampleRate) / decimation;
	static constexpr uint32_t lineLen[4] = {211, 263, 331, 409};		// samples at 6 kHz (35 - 68 ms)
	float line[4][409];
	uint32_t pos[4];
	uint32_t clearPos = 0;				// lines are cleared a few at a time (a 6.5 kB memset in the audio interrupt overran it)
	uint32_t phase = 0;
	float preLp = 0.0f;					// anti-alias low pass before decimation
	float heldL = 0.0f, heldR = 0.0f, smoothL = 0.0f, smoothR = 0.0f;
	float hpStateL = 0.0f, hpStateR = 0.0f, hpLastL = 0.0f, hpLastR = 0.0f;
	float feedback = 0.86f, gain = 1.0f, norm = 1.0f, width = 0.4f, hpCoef = 0.97f;
	stmlib::Svf lpL, lpR;
};


// Kick body for the rumble engine (modulove, own design, controls modelled on what a modern kick voice offers):
// sine with exponential pitch sweep (curve: round 808-like ... snappy 909-like), sustain plateau for perceived
// loudness, self-FM colour and a transient synth (pitched knock + band-passed noise). Also used for the ghost taps.
class KickBody {
public:
	struct Params {
		float f0 = 0.001f;				// Final pitch (normalised frequency)
		float sweepOct = 1.0f;			// Pitch envelope depth (octaves)
		float sweepCoef = 0.999f;		// Pitch envelope decay per sample
		uint32_t holdSamples = 0;		// Sustain plateau length
		float holdCoef = 1.0f;			// Amplitude decay during the plateau
		float decayCoef = 0.999f;		// Amplitude decay after the plateau
		float color = 0.0f;				// Self FM (0 - 1)
		float click = 0.5f;				// Transient level
		float clickCoef = 0.99f;		// Transient decay per sample
		float clickF = 0.01f;			// Transient knock frequency (normalised)
		float clickNoiseF = 0.04f;		// Transient noise band centre (normalised)
	};
	void Init();
	void Trigger(const Params& np, const float accent);
	float Process();
	float Level() const { return amp; }
private:
	Params p;
	float phase = 0.0f, penv = 0.0f, amp = 0.0f;
	uint32_t hold = 0;
	float fb1 = 0.0f, fb2 = 0.0f;
	float clickEnv = 0.0f, knockPhase = 0.0f;
	float residual = 0.0f, last = 0.0f;
	stmlib::Svf noiseBp;
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
	void CalcRumble(float& outL, float& outR);
	KickBody::Params BodyParams(const float lengthKnob, const float lengthScale, const float clickScale);
	float BodyFx(const float x);
	void ConfigureRumble();
	void CheckIdle(const float magnitude, const bool busy);

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

	// FM engine state (coefficients are set per hit, not per sample)
	float fmPhaseC = 0.0f, fmPhaseM = 0.0f, fmAmp = 0.0f, fmPitchEnv = 0.0f, fmIndexEnv = 0.0f;
	float fmPitchCoef = 0.999f, fmIndexCoef = 0.999f, fmAmpCoef = 0.999f, fmIndex = 0.0f;

	// Idle skip: engines that render continuously stop when silent and restart on the next hit
	static constexpr uint32_t quietLimit = 4800;		// 100 ms below -94 dBFS
	uint32_t quietCount = 0;
	bool idle = false;

	// Rumble engine state
	KickBody body;
	KickBody ghost;						// Ghost taps (Groove style: a copy of the kick on the next 16ths)
	Rumble rumbleFx;
	int32_t tapCountdown[3] = {-1, -1, -1};
	float noiseEnv = 0.0f, noiseCoef = 0.999f, noiseLp = 0.0f;
	float subPhase = 0.0f, subEnv = 0.0f, subPeak = 0.0f, subDecayCoef = 0.999f;
	float bodySweepOct = 1.0f, bodySweepCoef = 0.999f, bodyClickCoef = 0.99f, bodyClickRatio = 6.0f, bodyNoiseF = 0.04f;	// from config
	bool subAttack = false;

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
		float click = 0.5f;						// Rumble body: transient level (field was 'rumblePunch' in the first rumble version)
		float tap2 = 0.6f;						// Rumble ghost taps on the 1st, 2nd, 3rd 16th after each kick
		float tap3 = 0.4f;
		float tap4 = 0.25f;

		// Rumble engine v2: kick body and groove space (Tone = transient tone, Drive = body FX amount)
		float sweep = 24.0f;					// Body pitch sweep (semitones at full curve)
		float curve = 0.5f;						// Pitch curve: 0 round/slow (808-like) ... 1 snappy/fast (909-like)
		float sustain = 0.35f;					// Body plateau before the decay: perceived loudness
		float color = 0.15f;					// Body self FM: sine -> saw-ish
		float clickDecay = 6.0f;				// Transient decay (ms)
		float fxMode = 0.0f;					// Body FX: 0 soft clip, 1 hard clip, 2 wavefold
		float tapLength = 0.4f;					// Ghost tap length (x body length)
		float tapDirect = 0.0f;					// Ghost taps heard directly (0 = only inside the rumble)
		float rumbleNoise = 0.2f;				// Noise bursts into the rumble space on each kick / tap
		float rumbleGrit = 0.4f;				// Drive before the reverb (classic rumble trick)
		float subLevel = 0.3f;					// Sub pulse on each tap, tuned to the kick
		float rumbleWidth = 0.4f;				// Rumble stereo width (mono sum unaffected)
		float rumbleHp = 30.0f;					// Rumble high pass (Hz)
	} config;
};
