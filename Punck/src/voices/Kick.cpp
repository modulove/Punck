#include <Kick.h>
#include <cmath>
#include <cstring>
#include "Filter.h"
#include "VoiceManager.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"

// ------------------------------------------------------------------------------------------------
// Rumble processor
// ------------------------------------------------------------------------------------------------
constexpr uint32_t Rumble::lineLen[4];

void Rumble::Init()
{
	memset(line, 0, sizeof(line));
	for (auto& p : pos) {
		p = 0;
	}
	phase = 0;
	preLp = held = smooth = 0.0f;
	lp.Init();
}


float Rumble::Process(const float in, const float feedback, const float cutoffHz, const float drive)
{
	preLp += 0.075f * (in - preLp);										// ~600 Hz anti-alias before decimation

	if (++phase >= decimation) {
		phase = 0;
		float o[4];
		for (uint32_t i = 0; i < 4; ++i) {
			o[i] = line[i][pos[i]];
		}
		// Energy preserving 4x4 Hadamard mix in the feedback path
		const float h0 = 0.5f * (o[0] + o[1] + o[2] + o[3]);
		const float h1 = 0.5f * (o[0] - o[1] + o[2] - o[3]);
		const float h2 = 0.5f * (o[0] + o[1] - o[2] - o[3]);
		const float h3 = 0.5f * (o[0] - o[1] - o[2] + o[3]);
		const float x = 0.5f * preLp;
		line[0][pos[0]] = x + feedback * h0;
		line[1][pos[1]] = x + feedback * h1;
		line[2][pos[2]] = x + feedback * h2;
		line[3][pos[3]] = x + feedback * h3;
		for (uint32_t i = 0; i < 4; ++i) {
			if (++pos[i] >= lineLen[i]) {
				pos[i] = 0;
			}
		}

		float y = 0.5f * (o[0] + o[1] + o[2] + o[3]);
		const float g = 1.0f + 8.0f * drive;
		y = stmlib::SoftClip(y * g) / (1.0f + 1.5f * drive);				// saturation adds harmonics the low pass shapes
		lp.set_f_q<stmlib::FREQUENCY_FAST>(std::min(cutoffHz, 1500.0f) / (systemSampleRate / decimation), 0.8f);
		held = lp.Process<stmlib::FILTER_MODE_LOW_PASS>(y);
	}
	smooth += 0.04f * (held - smooth);									// smooth the 6 kHz steps
	return smooth;
}


// ------------------------------------------------------------------------------------------------
// Kick
// ------------------------------------------------------------------------------------------------
static inline float Saturate(const float x, const float drive)
{
	if (drive <= 0.0f) {
		return x;
	}
	const float g = 1.0f + 6.0f * drive;
	return stmlib::SoftClip(x * g) / (1.0f + 2.0f * drive);
}


void Kick::Play(const uint8_t voice, const uint32_t noteOffset, const uint32_t noteRange, const float velocity, const float pitch)
{
	playing = true;
	velocityScale = velocity * (static_cast<float>(ADC_array[ADC_KickLevel]) / 32768.0f);
	notePitch = pitch;
	accent = std::clamp(velocity, 0.0f, 1.0f);
	trigger = true;

	switch (engine) {
	case classic:
		phase = Phase::Ramp1;
		position = 0.0f;
		currentLevel = 0.0f;
		slowSinInc = FreqToInc(config.initSlowSinFreq * pitch);
		slowSinLevel = 1.0f;
		break;

	case fm:
		fmPhaseC = 0.25f;				// start on the sine peak: click
		fmPhaseM = 0.0f;
		fmAmp = 1.0f;
		fmPitchEnv = 1.0f;
		fmIndexEnv = 1.0f;
		break;

	case rumble: {
		// Ghost taps on the next three 16ths, fed into the rumble only (BOHM 'Groove' style)
		const float sixteenth = voiceManager.SixteenthSamples();
		const float levels[3] = {config.tap2, config.tap3, config.tap4};
		for (uint32_t i = 0; i < 3; ++i) {
			tapCountdown[i] = (sixteenth > 0.0f && levels[i] > 0.0f) ? static_cast<int32_t>(sixteenth * (i + 1)) : -1;
		}
		break;
	}

	default:
		break;
	}

	voiceManager.KickTriggered(velocity);		// Sidechain envelope (used by the rumble; other voices only if enabled)
}


void Kick::Play(const uint8_t voice, const uint32_t index)
{
	// Called when button is pressed
	Play(0, 0, 0, 1.0f, 1.0f);
}


void Kick::CalcClassic()
{
	switch (phase) {
	case Phase::Ramp1:
		currentLevel = currentLevel + (config.ramp1Inc * (1.0f - currentLevel));
		if (currentLevel > 0.6f) {
			phase = Phase::Ramp2;
		}
		break;

	case Phase::Ramp2:
		currentLevel = currentLevel + (config.ramp2Inc * (1.0f - currentLevel));
		if (currentLevel > 0.82f) {
			currentLevel = 0.78f;					// discontinuity sharply falls at first
			phase = Phase::Ramp3;
		}
		break;

	case Phase::Ramp3:
		currentLevel = currentLevel + (config.ramp3Inc * (1.0f - currentLevel));
		if (currentLevel > 0.93f) {
			position = 2.0f;
			phase = Phase::FastSine;
		}
		break;

	case Phase::FastSine:
		position += fastSinInc * notePitch;
		currentLevel = std::sin(position);
		if (position >= 1.5f * pi) {
			phase = Phase::SlowSine;
		}
		break;

	case Phase::SlowSine:
	{
		slowSinInc *= config.sineSlowDownRate;			// Sine wave slowly decreases in frequency
		position += slowSinInc;							// Set current poition in sine wave
		const float decaySpeed = 0.9994f + 0.00055f * static_cast<float>(ADC_array[ADC_KickDecay]) / 65536.0f;
		slowSinLevel = slowSinLevel * decaySpeed;
		currentLevel = std::sin(position) * slowSinLevel;

		if (slowSinLevel <= 0.00001f) {
			playing = false;
			phase = Phase::Off;
		}
		break;
	}

	default:
		break;
	}

	if (phase != Phase::Off) {
		noteMapper->pwmLed.Level(slowSinLevel);
		outputLevel[0] = velocityScale * filter.CalcFilter(currentLevel, left);
		outputLevel[1] = outputLevel[0];
	}
}


float Kick::CalcFm()
{
	// 2-operator FM kick: sine carrier with exponential pitch drop, modulator index envelope = attack grit
	const float f0 = config.pitch * notePitch / systemSampleRate;
	const float pitchCoef = std::exp(-1.0f / (std::max(config.fmPitchDecay, 1.0f) * 48.0f));
	const float indexCoef = std::exp(-1.0f / ((4.0f + 40.0f * knobA) * 48.0f));
	const float t60 = 80.0f * std::pow(2500.0f / 80.0f, knobB);						// 80 ms .. 2.5 s
	const float ampCoef = std::exp(-6.9078f / (t60 * 48.0f));

	const float f = std::min(f0 * (1.0f + config.fmPitchEnv * fmPitchEnv), 0.2f);
	fmPitchEnv *= pitchCoef;
	fmIndexEnv *= indexCoef;
	fmAmp *= ampCoef;

	fmPhaseM += f * config.fmRatio;
	fmPhaseM -= static_cast<float>(static_cast<int32_t>(fmPhaseM));
	fmPhaseC += f;
	fmPhaseC -= static_cast<float>(static_cast<int32_t>(fmPhaseC));

	const float index = 5.0f * knobA * fmIndexEnv;
	const float mod = plaits::Sine(fmPhaseM) * index * 0.159155f;					// radians -> cycles
	const float out = plaits::Sine(fmPhaseC + mod + 4.0f) * fmAmp;
	return Saturate(out, config.drive);
}


void Kick::CalcOutput()
{
	// Smooth the pots (they are also read for engines that use them as continuous parameters)
	knobA += 0.002f * (static_cast<float>(ADC_array[ADC_KickAttack]) / 65536.0f - knobA);
	knobB += 0.002f * (static_cast<float>(ADC_array[ADC_KickDecay]) / 65536.0f - knobB);

	if (engine == classic) {
		CalcClassic();
		return;
	}

	const float level = static_cast<float>(ADC_array[ADC_KickLevel]) / 32768.0f;
	const float f0 = std::min(config.pitch * notePitch / systemSampleRate, 0.1f);
	float out = 0.0f;

	switch (engine) {
	case deep: {
		// Plaits bass drum mapping: one 'harmonics' macro = attack FM -> self FM -> drive
		const float h = knobA;
		const float attackFm = std::min(h * 4.0f, 1.0f);
		const float selfFm = std::clamp(h * 4.0f - 1.0f, 0.0f, 1.0f);
		const float drive = std::max(h * 2.0f - 1.0f, 0.0f) * std::max(1.0f - 16.0f * f0, 0.0f);
		analogBd.Render(false, trigger, accent, f0, config.tone, knobB, attackFm, selfFm, &out, 1);
		out = Saturate(out, drive);
		break;
	}

	case punch: {
		const float h = knobA;
		syntheticBd.Render(false, trigger, accent, f0, config.tone, knobB, 0.4f - 0.25f * knobB * knobB,
				std::min(h * 2.0f, 1.0f), std::max(h * 2.0f - 1.0f, 0.0f), &out, 1);
		break;
	}

	case fm:
		out = CalcFm();
		break;

	case rumble: {
		const float h = config.rumblePunch;
		float body = 0.0f, ghost = 0.0f;
		analogBd.Render(false, trigger, accent, f0, config.tone, knobB, std::min(h * 4.0f, 1.0f),
				std::clamp(h * 4.0f - 1.0f, 0.0f, 1.0f), &body, 1);

		for (auto& t : tapCountdown) {
			if (t > 0 && --t == 0) {
				ghostTrigger = true;
				const uint32_t i = &t - tapCountdown;
				ghostAccent = accent * (i == 0 ? config.tap2 : i == 1 ? config.tap3 : config.tap4);
				t = -1;
			}
		}
		ghostBd.Render(false, ghostTrigger, ghostAccent, f0, config.tone * 0.6f, knobB * 0.8f, 0.2f, 0.0f, &ghost, 1);
		ghostTrigger = false;

		const float rum = rumbleFx.Process(body + ghost, std::clamp(config.rumbleFeedback, 0.0f, 0.97f),
				config.rumbleCutoff, config.rumbleDrive);
		const float gain = knobA * config.rumbleLevel * (1.0f - config.rumbleDuck * voiceManager.duckLevel);
		out = Saturate(body, config.drive) + rum * gain;
		break;
	}

	default:
		break;
	}
	trigger = false;

	outputLevel[0] = outputLevel[1] = level * out;
	ledEnv = std::max(std::abs(out), ledEnv * 0.9997f);
	noteMapper->pwmLed.Level(std::min(ledEnv, 1.0f));
}


void Kick::SetEngine(const uint8_t e)
{
	engine = static_cast<Engine>(e < engineCount ? e : 0);
	config.engine = engine;
	analogBd.Init();
	syntheticBd.Init();
	ghostBd.Init();
	rumbleFx.Init();
	for (auto& t : tapCountdown) {
		t = -1;
	}
	phase = Phase::Off;
	playing = (engine != classic);				// sample based engines run continuously
	trigger = false;
	outputLevel[0] = outputLevel[1] = 0.0f;
}


void Kick::UpdateFilter()
{
	filter.Update(false);
}


uint32_t Kick::SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex)
{
	config.engine = engine;
	*buff = reinterpret_cast<uint8_t*>(&config);
	return sizeof(config);
}


void Kick::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff != nullptr && len <= sizeof(config)) {
		memcpy(&config, buff, len);
	}

	// Apply any settings that are constant until configuration changes
	fastSinInc = FreqToInc(config.fastSinFreq);
	const uint8_t e = static_cast<uint8_t>(std::clamp(config.engine, 0.0f, static_cast<float>(engineCount - 1)));
	if (buff == nullptr || e != engine) {
		SetEngine(e);
	}
}

uint32_t Kick::ConfigSize()
{
	return sizeof(config);
}
