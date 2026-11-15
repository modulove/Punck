#include "Snare.h"
#include "VoiceManager.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"
#include <cmath>
#include <cstring>

void Snare::Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch)
{
	playing = true;
	noteRange = noteRange == 0 ? 128 : noteRange;
	const float tuning = static_cast<float>(ADC_array[ADC_SnareTuning]) / 65536.0f;

	if (engine == classic) {
		partialpos[0] = config.basePos;						// Create discontinuity to create initial click
		partialpos[1] = 0.0f;
		partialpos[2] = 0.0f;

		const float freq = pitch * (config.baseFreq * (tuning + 0.5f));
		for (uint8_t i = 0; i < partialCount; ++i) {
			partialLevel[i] = config.partialInitLevel[i];
			partialInc[i] = FreqToInc(freq * config.partialFreqOffset[i]);
		}
		noiseLevel = config.noiseInitLevel;
		velocityScale = velocity * (static_cast<float>(ADC_array[ADC_SnareLevel]) / 32768.0f);
		sustainRate = 0.0012f * sqrt((static_cast<float>(noteOffset) + 1.0f) / noteRange);		// note offset allows for longer sustained hits
		return;
	}

	// Sample based engines: Tuning knob = x0.5 .. x2 around config pitch, Filter knob = snappy
	const float hz = config.pitch * pitch * std::exp2((tuning - 0.5f) * 2.0f);
	f0 = std::min(hz / systemSampleRate, 0.15f);
	snappy = static_cast<float>(ADC_array[ADC_SnareFilter]) / 65536.0f;
	accent = std::clamp(velocity, 0.0f, 1.0f);
	decayEff = std::clamp(config.decay + 0.4f * std::min(static_cast<float>(noteOffset) / noteRange, 1.0f), 0.0f, 1.0f);
	trigger = true;

	if (engine == rim) {
		const float ratios[2] = {1.0f, config.rimRatio};
		const float decayMs[2] = {config.rimDecay * (0.6f + decayEff), config.rimDecay * 0.6f * (0.6f + decayEff)};
		for (uint32_t i = 0; i < 2; ++i) {
			rimPhase[i] = 0.25f;										// start at peak for a hard edge
			rimInc[i] = std::min(f0 * ratios[i], 0.45f);
			rimEnv[i] = (i == 0 ? 1.0f : 0.6f) * accent;
			rimCoef[i] = std::exp(-6.9078f / (std::max(decayMs[i], 2.0f) * 48.0f));
		}
		clickEnv = accent * (0.3f + 0.7f * snappy);
		clickCoef = std::exp(-6.9078f / ((2.0f + 25.0f * snappy) * 48.0f));
	}
}


void Snare::Play(const uint8_t voice, const uint32_t index)
{
	// Called when button is pressed
	Play(0, index, 0, 1.0f, 1.0f);
}


void Snare::CalcClassic()
{
	if (!playing) {
		return;
	}
	const float rand1 = intToFloatMult * static_cast<int32_t>(RNG->DR);		// Left channel random number used for noise

	noiseLevel *= config.noiseDecay + sustainRate;
	float maxLevel = noiseLevel;

	float partialOutput = 0.0f;
	for (uint8_t i = 0; i < partialCount; ++i) {
		partialpos[i] += partialInc[i];							// Set current poition in sine wave
		partialLevel[i] *= config.partialDecay + sustainRate;
		partialOutput += std::sin(partialpos[i]) * partialLevel[i];
		if (partialLevel[i] > maxLevel) {
			maxLevel = partialLevel[i];
		}
	}

	const float rand2 = intToFloatMult * static_cast<int32_t>(RNG->DR);		// Get right channel noise value: calc here to give time for peripheral to update

	outputLevel[left]  = velocityScale * filter.CalcFilter(partialOutput + (rand1 * noiseLevel), left);
	outputLevel[right] = velocityScale * filter.CalcFilter(partialOutput + (rand2 * noiseLevel), right);

	if (maxLevel < 0.00001f) {
		playing = false;
	}
	noteMapper->pwmLed.Level(maxLevel);
}


void Snare::CalcOutput()
{
	if (engine == classic) {
		CalcClassic();
		return;
	}

	float out = 0.0f;
	switch (engine) {
	case analog:
		analogSd.Render(false, trigger, accent, f0, config.tone, decayEff, snappy, &out, 1);
		break;

	case synthetic:
		syntheticSd.Render(false, trigger, accent, f0, config.tone, decayEff, snappy, &out, 1);
		break;

	case rim: {
		for (uint32_t i = 0; i < 2; ++i) {
			rimPhase[i] += rimInc[i];
			rimPhase[i] -= static_cast<float>(static_cast<int32_t>(rimPhase[i]));
			out += plaits::Sine(rimPhase[i]) * rimEnv[i];
			rimEnv[i] *= rimCoef[i];
		}
		out += clickEnv * intToFloatMult * static_cast<int32_t>(RNG->DR);
		clickEnv *= clickCoef;
		break;
	}

	default:
		break;
	}
	trigger = false;

	const float level = static_cast<float>(ADC_array[ADC_SnareLevel]) / 32768.0f;
	outputLevel[left] = outputLevel[right] = level * out;
	ledEnv = std::max(std::abs(out), ledEnv * 0.9995f);
	noteMapper->pwmLed.Level(std::min(ledEnv, 1.0f));
}


void Snare::SetEngine(const uint8_t e)
{
	engine = static_cast<Engine>(e < engineCount ? e : 0);
	config.engine = engine;
	analogSd.Init();
	syntheticSd.Init();
	rimEnv[0] = rimEnv[1] = clickEnv = 0.0f;
	playing = (engine != classic);
	trigger = false;
	outputLevel[0] = outputLevel[1] = 0.0f;
}


void Snare::UpdateFilter()
{
	filter.Update(false);
}


uint32_t Snare::SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex)
{
	config.engine = engine;
	*buff = reinterpret_cast<uint8_t*>(&config);
	return sizeof(config);
}


void Snare::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff != nullptr && len <= sizeof(config)) {
		memcpy(&config, buff, len);
	}
	const uint8_t e = static_cast<uint8_t>(std::clamp(config.engine, 0.0f, static_cast<float>(engineCount - 1)));
	if (buff == nullptr || e != engine) {
		SetEngine(e);
	}
}


uint32_t Snare::ConfigSize()
{
	return sizeof(config);
}
