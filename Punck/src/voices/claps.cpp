#include "Claps.h"
#include "VoiceManager.h"

void Claps::Play(const uint8_t voice, const uint32_t noteOffset, uint32_t noteRange, const float velocity, const float pitch)
{
	playing = true;

	velocityScale = velocity;
	level = config.initLevel;
	state = State::hit1;
	stateCounter = 0;

	// modulove: band pass kept between 80 Hz and 18 kHz (Q 0.3 - 20) and started from rest on every hit. Chromatic
	// notes moved the cutoff from ~40 Hz to 22 kHz while the previous clap still rang, and the old filter state then
	// excited the new narrow resonator: up to +37 dB into the master bus on the module, +56 dB in an offline test
	// (stress test 2026-11-16). A clap starts from noise, so clearing the state is inaudible.
	const float omega = std::clamp(2.0f * config.filterCutoff * pitch / systemSampleRate, 2.0f * 80.0f / systemSampleRate, 0.75f);
	filter.CalcCoeff(omega, std::clamp(config.filterQ, 0.3f, 20.0f));	// omega = cutoff in Hz / half sampling frequency
	iirReg.Init();
}


void Claps::Play(const uint8_t voice, const uint32_t index)
{
	// Called when button is pressed
	Play(0, index, 0, 1.0f, 1.0f);
}


void Claps::CalcOutput()
{
	if (playing) {
		float output = intToFloatMult * static_cast<int32_t>(RNG->DR);		// Initial random number used for noise
		output = filter.FilterSample(output, iirReg);

		++stateCounter;

#ifdef DEBUGFILTER			// will just output filtered white noise for a few seconds to allow frequency analysis of filter
	if (stateCounter == 1000000) {
		playing = false;
	}

#else
		switch (state) {
		case State::hit1:
			if (stateCounter > 460) {		// approx 9.6ms
				level = config.initLevel;
				stateCounter = 0;
				state = State::hit2;
			}
			break;
		case State::hit2:
			if (stateCounter > 547) {		// approx 11.4ms
				level = config.initLevel;
				stateCounter = 0;
				state = State::hit3;
			}
			break;
		case State::hit3:
			if (stateCounter > 336) {		// approx 7ms
				level = config.reverbInitLevel;
				stateCounter = 0;
				state = State::reverb;
			}
			break;
		default:
			break;
		}

		output += config.unfilteredNoiseLevel * intToFloatMult * static_cast<int32_t>(RNG->DR);		// Add some additional non-filtered noise back in

		level *= (state == State::reverb ? config.reverbDecay : config.initDecay);
#endif
		outputLevel[left]  = velocityScale * (output * level);
		outputLevel[right] = outputLevel[left];

		if (level < 0.00001f && state == State::reverb) {
			playing = false;
		}

	}
}


void Claps::UpdateFilter()
{

}


uint32_t Claps::SerialiseConfig(uint8_t** buff, const uint8_t voiceIndex)
{
	*buff = reinterpret_cast<uint8_t*>(&config);
	return sizeof(config);
}


void Claps::StoreConfig(uint8_t* buff, const uint32_t len)
{
	if (buff != nullptr && len <= sizeof(config)) {
		memcpy(&config, buff, len);
	}
}


uint32_t Claps::ConfigSize()
{
	return sizeof(config);
}
