#include <HiHat.h>
#include "VoiceManager.h"
#include <cstring>
#include <cmath>
#include "Dynamics.h"
#include "stmlib/utils/random.h"


void HiHat::Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch)
{
	playing = true;

	velocityScale = velocity * voiceManager.LevelGain(VoiceManager::potHats);
	attack = true;
	attackLevel = 0.0f;
	noiseScale = intToFloatMult * config.noiseInitLevel;		// noise uses RND peripheral: scale 32 bit value to -1.0 to +1.0

	hpFilter.Init();
	hpFilterCutoff = config.hpInitCutoff;

	lpFilter.Init();
	lpFilterCutoff = config.lpInitCutoff;

	// Decay pot: the top zone randomises the decay per hit (mostly short, a few sustained - more towards full CW)
	const float r1 = stmlib::Random::GetFloat(), r2 = stmlib::Random::GetFloat();
	const float pot = RandomZone(static_cast<float>(ADC_array[ADC_HiHatDecay]) / 65536.0f, config.randomZone,
			config.randomLongMin, config.randomLongMax, config.randomBase, r1, r2);

	// Note index sets how open the hat is (higher MIDI notes / open trigger = more open); pot shifts by +/-0.5
	noteRange = noteRange == 0 ? 128 : noteRange;
	const float open = std::sqrt((static_cast<float>(noteOffset) + 1.0f) / noteRange) + pot - 0.5f;
	decayScale = std::min(config.decayMin + (config.decayRange * open), config.decayMax);

	if (engine != classic) {
		// Plaits hats: open amount (-0.41 closed .. 1.5 fully open) mapped onto Plaits decay
		morph = std::clamp(config.plaitsDecayBase + config.plaitsDecayScale * open, 0.0f, std::clamp(config.plaitsDecayMax, 0.0f, 1.0f));
		f0 = std::min(voiceManager.Tune(VoiceManager::hihat, config.pitch * pitch) / systemSampleRate, 0.1f);
		accent = std::clamp(velocity, 0.0f, 1.0f);
		trigger = true;
		idle = false;
		quietCount = 0;
		return;
	}

	// Ring modulated metallic layer fades in on open hats only, closed hats stay crisp
	const float ringAmt = (open - config.ringModOpenStart) / (1.0f - config.ringModOpenStart);
	ringLevel = config.ringModLevel * std::clamp(ringAmt, 0.0f, 1.0f);
	gritCounter = 0;
	gritSample = 0.0f;

	for (uint8_t i = 0; i < 6; ++i) {
		partialPos[i] = 0;
		partialLevel[i] = config.partialScale[i];
		partialPeriod[i] = static_cast<uint32_t>(freqToSqPeriod / (config.partialFreq[i] * pitch));
	}
}


void HiHat::Play(const uint8_t voice, const uint32_t index)
{
	// Called when button is pressed
	Play(0, index, 0, 1.0f, 1.0f);
}



void HiHat::CalcOutput()
{
	if (engine == classic) {
		CalcClassic();
		return;
	}

	if (idle && !trigger) {
		outputLevel[0] = outputLevel[1] = 0.0f;
		ledEnv = 0.0f;
		noteMapper->pwmLed.Level(0.0f);
		return;
	}

	float out = 0.0f, temp1 = 0.0f, temp2 = 0.0f;
	if (engine == square808) {
		hat808.Render(false, trigger, accent, f0, config.tone, morph, config.noisiness, &temp1, &temp2, &out, 1);
	} else {
		hat909.Render(false, trigger, accent, f0, config.tone, morph, config.noisiness, &temp1, &temp2, &out, 1);
	}
	trigger = false;

	const float level = voiceManager.LevelGain(VoiceManager::potHats);
	outputLevel[0] = outputLevel[1] = level * out;
	ledEnv = std::max(std::abs(out) * 2.0f, ledEnv * 0.9995f);
	noteMapper->pwmLed.Level(std::min(ledEnv, 1.0f));

	if (std::abs(out) < 2e-5f) {
		if (++quietCount > quietLimit) {
			idle = true;
		}
	} else {
		quietCount = 0;
	}
}


void HiHat::SetEngine(const uint8_t e)
{
	engine = static_cast<Engine>(e < engineCount ? e : 0);
	config.engine = engine;
	if (engine == square808) {
		hat808.Init();
	} else if (engine == ringMod909) {
		hat909.Init();
	}
	idle = false;
	quietCount = 0;
	playing = (engine != classic);
	trigger = false;
	outputLevel[0] = outputLevel[1] = 0.0f;
}


void HiHat::CalcClassic()
{
	if (playing) {

		const int32_t noise = static_cast<int32_t>(RNG->DR);		// Get noise level here to give time for next noise value to be calculated

		float currentLevel[2] = {};				// Store intermediate values of sample level for each channel

		for (uint8_t i = 0; i < 6; ++i) {
			if (config.partialFM[i] > 0 && partialLevel[0] > 0.0f) {			// Add some frequency modulation to some partials
				partialPos[i] += config.partialFM[i];
			} else {
				++partialPos[i];
			}

			if (partialPos[i] > partialPeriod[i]) {
				partialPos[i] = 0;
				partialLevel[i] = -partialLevel[i];
			}

			currentLevel[left] += partialLevel[i];
		}

		// Ring modulation: products of square partial pairs give dense inharmonic sidebands; sample & hold adds aliasing grit
		if (ringLevel > 0.0f) {
			if (++gritCounter >= static_cast<uint32_t>(config.gritHold)) {
				gritCounter = 0;
				gritSample = partialLevel[0] * partialLevel[3] + partialLevel[1] * partialLevel[4] + partialLevel[2] * partialLevel[5];
			}
			currentLevel[left] += 2.0f * ringLevel * gritSample;
		}


		// Apply an envelope to the HP filter
		if (hpFilterCutoff < config.hpFinalCutoff) {
			hpFilterCutoff *= config.hpCutoffInc;
		}

		// Apply an envelope to the LP filter
		if (lpFilterCutoff > config.lpFinalCutoff) {
			lpFilterCutoff *= config.lpCutoffInc;

		}


		// Add a burst of noise at the beginning of the note (both channels use the sample partials, but different noise)
		noiseScale *= config.noiseDecay;
		currentLevel[left] += noiseScale * noise;
		currentLevel[right] = currentLevel[left] + noiseScale * (int32_t)RNG->DR;


		// Apply a short attack fade in then longer decay
		float envScale;
		if (attack) {
			attackLevel += config.attackInc;
			if (attackLevel >= velocityScale) {
				attackLevel = velocityScale;
				attack = false;
			}
			envScale = attackLevel;
		} else {
			velocityScale *= decayScale;
			envScale = velocityScale;
		}


		// End when note volume is 0
		if (velocityScale < 0.001f) {
			playing = false;
			currentLevel[left] = 0.0f;
			currentLevel[right] = 0.0f;
			hpFilterCutoff = config.hpInitCutoff;			// Reset filter cutoff so initial cofficients can be recalculated in idle time
			lpFilterCutoff = config.lpInitCutoff;
		}

		// Filter and scale the output levels
		outputLevel[0] = envScale * lpFilter.CalcFilter(hpFilter.CalcFilter(currentLevel[left], left), left) - 0.0001f;
		outputLevel[1] = envScale * lpFilter.CalcFilter(hpFilter.CalcFilter(currentLevel[right], right), right) - 0.0001f;

		noteMapper->pwmLed.Level(velocityScale);
	}

}


void HiHat::UpdateFilter()
{
	hpFilter.SetCutoff(hpFilterCutoff);
	lpFilter.SetCutoff(lpFilterCutoff);
}


uint32_t HiHat::SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex)
{
	config.engine = engine;
	*buff = reinterpret_cast<uint8_t*>(&config);
	return sizeof(config);
}


FLASH_CODE void HiHat::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff != nullptr && len <= sizeof(config)) {
		memcpy(&config, buff, len);
	}

	// Apply any settings that are constant until configuration changes
	for (uint8_t i = 0; i < 6; ++i) {
		partialPeriod[i] = freqToSqPeriod / config.partialFreq[i];
	}
	config.gritHold = std::max(config.gritHold, 1.0f);

	const uint8_t e = static_cast<uint8_t>(std::clamp(config.engine, 0.0f, static_cast<float>(engineCount - 1)));
	if (buff == nullptr || e != engine) {
		SetEngine(e);
	}
}


uint32_t HiHat::ConfigSize()
{
	return sizeof(config);
}
