#include "Snare.h"
#include "VoiceManager.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"
#include <cmath>
#include "Dynamics.h"
#include <cstring>

// String engine delay lines (3 strings x 5 kB): DTCM (.bss) - zero wait states and never evicted from the cache
// by the reverb (in AXI SRAM the snare string cost ~990 cycles/sample with 3 strings ringing)
static float stringBuffer[KarplusString::bufferFloats];

void Snare::Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch)
{
	playing = true;
	noteRange = noteRange == 0 ? 128 : noteRange;
	const float tuning = static_cast<float>(ADC_array[ADC_SnareTuning]) / 65536.0f;

	if (engine == classic) {
		partialpos[0] = config.basePos;						// Create discontinuity to create initial click
		partialpos[1] = 0.0f;
		partialpos[2] = 0.0f;

		const float freq = voiceManager.Tune(VoiceManager::snare, pitch * (config.baseFreq * (tuning + 0.5f)));	// Tuning pot steps through the key
		for (uint8_t i = 0; i < partialCount; ++i) {
			partialLevel[i] = config.partialInitLevel[i];
			partialInc[i] = FreqToInc(freq * config.partialFreqOffset[i]);
		}
		noiseLevel = config.noiseInitLevel;
		velocityScale = velocity * voiceManager.LevelGain(VoiceManager::potSnare);
		// Note offset allows for longer sustained hits. modulove: the per-sample factor stays below 1 - decay +
		// sustainRate went above 1 with long Tone Lab decays (> ~120 ms with MIDI note 38) and the level grew every
		// sample until inf / nan (stress test 2026-11-16: +107 dB, then a silent module with DC on the output)
		const float sustainRate = 0.0012f * sqrt((static_cast<float>(noteOffset) + 1.0f) / noteRange);
		noiseFactor = std::min(config.noiseDecay + sustainRate, maxDecayFactor);
		partialFactor = std::min(config.partialDecay + sustainRate, maxDecayFactor);
		return;
	}

	// Sample based engines: Tuning knob = x0.5 .. x2 around config pitch, Filter knob = snappy
	const float hz = voiceManager.Tune(VoiceManager::snare, config.pitch * pitch * dyn::FastExp2((tuning - 0.5f) * 2.0f));
	f0 = std::min(hz / systemSampleRate, 0.15f);
	snappy = static_cast<float>(ADC_array[ADC_SnareFilter]) / 65536.0f;
	accent = std::clamp(velocity, 0.0f, 1.0f);
	idle = false;
	quietCount = 0;
	decayEff = std::clamp(config.decay + 0.4f * std::min(static_cast<float>(noteOffset) / noteRange, 1.0f), 0.0f, 1.0f);
	trigger = true;

	if (engine == rim) {
		const float ratios[2] = {1.0f, config.rimRatio};
		const float decayMs[2] = {config.rimDecay * (0.6f + decayEff), config.rimDecay * 0.6f * (0.6f + decayEff)};
		for (uint32_t i = 0; i < 2; ++i) {
			rimPhase[i] = 0.25f;										// start at peak for a hard edge
			rimInc[i] = std::min(f0 * ratios[i], 0.45f);
			rimEnv[i] = (i == 0 ? 1.0f : 0.6f) * accent;
			rimCoef[i] = dyn::DecayCoef(std::max(decayMs[i], 2.0f));
		}
		clickEnv = accent * (0.3f + 0.7f * snappy);
		clickCoef = dyn::DecayCoef(2.0f + 25.0f * snappy);
	}

	if (engine == modal && modalReady) {
		// Filter pot = mallet brightness; coefficients for all modes are computed here, once per hit
		modalDrum.set_num_modes(static_cast<int>(config.modalModes));
		modalDrum.Strike(f0, std::clamp(config.modalStructure, 0.0f, 1.0f), snappy, decayEff, accent);
		wireEnv = accent * std::clamp(config.modalWires, 0.0f, 2.0f);
		wireCoef = dyn::DecayCoef(std::max(config.modalWireDecay, 5.0f) * (0.5f + decayEff));
		wireBp.set_f_q<stmlib::FREQUENCY_FAST>(std::clamp(config.modalWireTone, 500.0f, 12000.0f) / systemSampleRate, 1.2f);
	}

	if (engine == bongo) {
		PlaitsSynth::Patch p;
		p.harmonics = config.bongoRatio;
		p.timbre = snappy;
		p.morph = config.bongoFeedback;
		p.decay = decayEff;
		p.colour = config.bongoColour;
		p.pitchEnv = config.bongoPitchEnv;
		p.timbreEnv = config.bongoFmEnv;
		synth.Trigger(hz, accent, p, PlaitsSynth::fm);
	}

	if (engine == karplus && stringReady) {
		// Filter pot = brightness (Plaits: timbre squared), decay = damping
		kString.Strike(f0, accent, std::clamp(config.stringStructure, 0.0f, 1.0f), snappy * snappy,
				std::min(decayEff, maxStringDamping), static_cast<uint32_t>(config.stringStrings + 0.5f));
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

	noiseLevel *= noiseFactor;
	float maxLevel = noiseLevel;

	float partialOutput = 0.0f;
	for (uint8_t i = 0; i < partialCount; ++i) {
		partialpos[i] += partialInc[i];							// Set current poition in sine wave
		partialLevel[i] *= partialFactor;
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

	if (idle && !trigger) {
		outputLevel[left] = outputLevel[right] = 0.0f;
		ledEnv = 0.0f;
		noteMapper->pwmLed.Level(0.0f);
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

	case modal: {
		float wires = 0.0f;
		if (wireEnv > 1e-5f) {
			wires = wireEnv * wireBp.Process<stmlib::FILTER_MODE_BAND_PASS_NORMALIZED>(intToFloatMult * static_cast<int32_t>(RNG->DR));
			wireEnv *= wireCoef;
		}
		out = config.modalLevel * (modalGain * modalDrum.Process(0.0f) + wires);
		break;
	}

	case karplus:
		out = config.stringLevel * stringGain * kString.Process();
		break;

	case bongo:
		out = config.bongoLevel * bongoGain * synth.Process();
		break;

	default:
		break;
	}
	trigger = false;

	const float level = voiceManager.LevelGain(VoiceManager::potSnare);
	outputLevel[left] = outputLevel[right] = level * out;
	ledEnv = std::max(std::abs(out), ledEnv * 0.9995f);
	noteMapper->pwmLed.Level(std::min(ledEnv, 1.0f));

	if (std::abs(out) < 2e-5f) {
		if (++quietCount > quietLimit) {
			idle = true;
		}
	} else {
		quietCount = 0;
	}
}


void Snare::SetEngine(const uint8_t e)
{
	engine = static_cast<Engine>(e < engineCount ? e : 0);
	config.engine = engine;
	if (engine == analog) {
		analogSd.Init();
	} else if (engine == synthetic) {
		syntheticSd.Init();
	} else if (engine == modal) {
		modalDrum.Reset();
		wireBp.Reset();
		wireEnv = 0.0f;
	}
	kString.Silence();
	synth.Silence();
	rimEnv[0] = rimEnv[1] = clickEnv = 0.0f;
	idle = false;
	quietCount = 0;
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


FLASH_CODE void Snare::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff == nullptr && !modalReady) {
		modalDrum.Init();						// start-up only: builds tables with pow/cos (too slow for the audio interrupt)
		wireBp.Init();
		modalReady = true;
		kString.Init(stringBuffer);
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


uint32_t Snare::ConfigSize()
{
	return sizeof(config);
}
