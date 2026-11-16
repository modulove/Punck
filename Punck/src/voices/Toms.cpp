#include <Toms.h>
#include <cmath>
#include <cstring>
#include "Filter.h"
#include "VoiceManager.h"
#include "Dynamics.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"

// Pluck engine delay lines (3 strings x 5 kB): AXI SRAM, cleared at start-up by KarplusString::Init
static float __attribute__((section (".ram_d1_data"))) stringBuffer[KarplusString::bufferFloats];


// Polynomial band-limited step correction (anti-aliased saw / pulse)
static inline float PolyBlep(float t, const float dt)
{
	if (t < dt) {
		t /= dt;
		return t + t - t * t - 1.0f;
	}
	if (t > 1.0f - dt) {
		t = (t - 1.0f) / dt;
		return t * t + t + t + 1.0f;
	}
	return 0.0f;
}


void Toms::Play(const uint8_t voice, const uint32_t noteOffset, const uint32_t noteRange, const float velocity, const float pitch)
{
	if (engine != classic) {
		if (noteRange == 0) {
			noteRatio = pitch;							// Chromatic channel: remembered for the auto notes after each kick
			Note(pitch, velocity);
		} else {
			Note(noteRatio * (noteOffset > 0 ? 2.0f : 1.0f), velocity);	// Drum map: high tom note = octave up
		}
		return;
	}

	playing = true;
	phase = Phase::Ramp;

	currentLevel = 0.0f;
	velocityScale = velocity;

	pitchScale = pitch * (1.0f + 1.5f * static_cast<float>(noteOffset) / (noteRange == 0 ? 128 : noteRange));
	pitchScale = voiceManager.Tune(VoiceManager::toms, config.baseFreq * pitchScale) / config.baseFreq;

	for (uint8_t i = 0; i < partialCount; ++i) {
		position[i] = 2.0f;
		sineInc[i] = config.sineFreqScale[i] * FreqToInc(config.baseFreq) * pitchScale;	// upstream used [1] for every partial
		sineLevel[i] = config.sineInitLevel[i];
	}
}


void Toms::Play(const uint8_t voice, const uint32_t index)
{
	// Called when button is pressed
	Play(0, 0, 0, 1.0f, 1.0f);
}


void Toms::BassNote(const float ratio, const float velocity)
{
	// Once per note: envelopes restart from their current level (no click on fast repeats), oscillator phase runs on
	oscInc = std::min(voiceManager.Tune(VoiceManager::toms, config.bassPitch * ratio) / systemSampleRate, 0.2f);
	ampPeak = std::clamp(velocity, 0.0f, 1.0f) * config.bassLevel;
	ampAttack = true;
	ampCoef = dyn::DecayCoef(std::max(config.bassDecay, 10.0f));
	filterEnv = 1.0f;
	filterCoef = dyn::DecayCoef(std::max(config.bassEnvDecay, 2.0f));
	const float a = std::clamp(config.bassAccent, 0.0f, 1.0f);
	envDepth = std::clamp(config.bassEnv, 0.0f, 8.0f) * (1.0f - a + a * std::clamp(velocity, 0.0f, 1.0f));
	idle = false;
	quietCount = 0;
	playing = true;
}


void Toms::PluckNote(const float ratio, const float velocity)
{
	if (!stringReady) {
		return;
	}
	const float f0 = std::clamp(voiceManager.Tune(VoiceManager::toms, config.pluckPitch * ratio) / systemSampleRate, 0.0005f, 0.2f);
	kString.Strike(f0, std::clamp(velocity, 0.0f, 1.0f), std::clamp(config.pluckStructure, 0.0f, 1.0f),
			std::clamp(config.pluckBrightness, 0.0f, 1.0f), std::clamp(config.pluckDecay, 0.0f, maxStringDamping),
			static_cast<uint32_t>(config.pluckStrings + 0.5f));
	idle = false;
	playing = true;
}


void Toms::SynthNote(const float ratio, const float velocity)
{
	PlaitsSynth::Patch p;
	p.harmonics = config.synthHarmonics;
	p.timbre = config.synthTimbre;
	p.morph = config.synthMorph;
	p.decay = config.synthDecay;
	p.colour = config.synthColour;
	p.pitchEnv = config.synthPitchEnv;
	p.timbreEnv = config.synthTimbreEnv;
	p.morphEnv = config.synthMorphEnv;
	const float hz = std::clamp(voiceManager.Tune(VoiceManager::toms, config.synthPitch * ratio), 8.0f, 12000.0f);
	synth.Trigger(hz, std::clamp(velocity, 0.0f, 1.0f), p, static_cast<PlaitsSynth::Model>(engine - synthFm));
	idle = false;
	playing = true;
}


void Toms::KickPlayed(const float sixteenthSamples, const float velocity)
{
	// Auto bass / pluck notes on the 16ths after each kick (psytrance rolling bass, offbeat bass, dub stabs ...)
	if (engine == classic) {
		return;
	}
	static constexpr uint8_t steps[6][3] = {{0, 0, 0}, {2, 0, 0}, {1, 2, 3}, {1, 2, 0}, {2, 3, 0}, {1, 3, 0}};
	const uint32_t pattern = std::clamp<int32_t>(static_cast<int32_t>(config.bassFollow + 0.5f), 0, 5);
	for (uint32_t i = 0; i < 3; ++i) {
		const uint8_t step = steps[pattern][i];
		followCountdown[i] = (step && sixteenthSamples > 0.0f) ? static_cast<int32_t>(sixteenthSamples * step) : -1;
	}
	followVelocity = std::clamp(velocity * config.bassFollowVel, 0.0f, 1.0f);
}


float Toms::CalcBass()
{
	// Oscillators: saw <-> pulse (poly BLEP), sub sine one octave down
	oscPhase += oscInc;
	if (oscPhase >= 1.0f) {
		oscPhase -= 1.0f;
	}
	const float t = oscPhase, dt = oscInc;
	const float saw = 2.0f * t - 1.0f - PolyBlep(t, dt);
	float t2 = t + 0.5f;
	if (t2 >= 1.0f) {
		t2 -= 1.0f;
	}
	const float square = (t < 0.5f ? 1.0f : -1.0f) + PolyBlep(t, dt) - PolyBlep(t2, dt);
	const float osc = saw + std::clamp(config.bassWave, 0.0f, 1.0f) * (square - saw);
	subPhase += 0.5f * oscInc;
	if (subPhase >= 1.0f) {
		subPhase -= 1.0f;
	}

	// Envelopes
	if (ampAttack) {
		ampEnv += 0.06f * (ampPeak - ampEnv);								// ~0.3 ms attack, no click
		if (ampEnv >= 0.97f * ampPeak) {
			ampAttack = false;
		}
	} else {
		ampEnv *= ampCoef;
	}
	filterEnv *= filterCoef;

	// 24 dB low pass: two SVFs, resonance on the second
	const float fc = std::clamp(config.bassCutoff * dyn::FastExp2(envDepth * filterEnv), 20.0f, 9000.0f) / systemSampleRate;
	const float reso = std::clamp(config.bassReso, 0.0f, 1.0f);
	lp1.set_f_q<stmlib::FREQUENCY_FAST>(fc, 0.707f);
	lp2.set_f_q<stmlib::FREQUENCY_FAST>(fc, 0.707f + 7.0f * reso * reso);
	float x = lp2.Process<stmlib::FILTER_MODE_LOW_PASS>(lp1.Process<stmlib::FILTER_MODE_LOW_PASS>(osc));
	x += config.bassSub * plaits::Sine(subPhase);								// sub after the filter: clean low end

	// Drive after the envelope: louder notes saturate more
	const float d = std::clamp(config.bassDrive, 0.0f, 1.0f);
	return stmlib::SoftClip(x * ampEnv * (1.0f + 3.0f * d)) / (1.0f + 0.8f * d);
}


void Toms::CalcOutput()
{
	if (engine == classic) {
		CalcToms();
		return;
	}

	for (auto& c : followCountdown) {
		if (c > 0 && --c == 0) {
			c = -1;
			Note(noteRatio, followVelocity);
		}
	}
	if (idle) {
		outputLevel[0] = outputLevel[1] = 0.0f;
		return;
	}
	if (engine == pluck) {
		outputLevel[0] = outputLevel[1] = config.pluckLevel * stringGain * kString.Process();
		if (!kString.Active()) {
			idle = true;
			playing = false;
		}
		return;
	}
	if (engine >= synthFm) {
		outputLevel[0] = outputLevel[1] = config.synthLevel * synthGain * synth.Process();
		if (!synth.Active()) {
			idle = true;
			playing = false;
		}
		return;
	}
	const float out = CalcBass();
	outputLevel[0] = outputLevel[1] = out;
	if (!ampAttack && ampEnv < 1e-5f) {
		idle = true;
		playing = false;
	}
}


void Toms::CalcToms()
{
	switch (phase) {
	case Phase::Ramp:
		currentLevel = currentLevel + (config.rampInc * (1.0f - currentLevel));

		if (currentLevel > 0.93f) {
			phase = Phase::Sine;
		}
		break;

	case Phase::Sine:
	{
		currentLevel = 0;
		for (uint8_t i = 0; i < partialCount; ++i) {
			sineInc[i] *= config.sineSlowDownRate;			// Sine wave slowly decreases in frequency
			position[i] += sineInc[i];						// Set current poition in sine wave
			sineLevel[i] *= config.decaySpeed[i];
			currentLevel += std::sin(position[i]) * sineLevel[i];
		}

		if (sineLevel[0] <= 0.00001f) {
			playing = false;
			phase = Phase::Off;
		}
		break;
	}

	default:
		break;
	}

	if (phase != Phase::Off) {
		outputLevel[0] = velocityScale * currentLevel;
		outputLevel[1] = outputLevel[0];
	}
}


void Toms::SetEngine(const uint8_t e)
{
	engine = static_cast<Engine>(e < engineCount ? e : 0);
	config.engine = engine;
	phase = Phase::Off;
	lp1.Init();
	lp2.Init();
	ampEnv = filterEnv = 0.0f;
	ampAttack = false;
	kString.Silence();
	synth.Silence();
	idle = true;
	for (auto& c : followCountdown) {
		c = -1;
	}
	outputLevel[0] = outputLevel[1] = 0.0f;
}


void Toms::UpdateFilter()
{

}


uint32_t Toms::SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex)
{
	config.engine = engine;
	*buff = reinterpret_cast<uint8_t*>(&config);
	return sizeof(config);
}


void Toms::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff == nullptr && !stringReady) {
		kString.Init(stringBuffer);				// start-up only: clears the delay lines
		stringReady = true;
		synth.Init();
	}
	if (buff != nullptr && len <= sizeof(config)) {
		memcpy(&config, buff, len);
	}
	const uint8_t e = static_cast<uint8_t>(std::clamp(config.engine, 0.0f, static_cast<float>(engineCount - 1)));
	if (buff == nullptr || e != engine) {
		SetEngine(e);
	}
}


uint32_t Toms::ConfigSize()
{
	return sizeof(config);
}
