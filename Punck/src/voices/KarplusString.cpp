#include "KarplusString.h"
#include <algorithm>
#include <cmath>
#include "stmlib/dsp/units.h"
#include "stmlib/utils/random.h"

void KarplusString::Init(float* buffer)
{
	for (uint32_t i = 0; i < maxStrings; ++i) {
		float* b = buffer + i * floatsPerString;
		voice[i].string.Init(b, b + plaits::kDelayLineSize);
		voice[i].excitation.Init();
		voice[i].active = false;
	}
	next = 0;
	limiter.Init();
}


void KarplusString::Silence()
{
	for (auto& v : voice) {
		v.active = false;
		v.noiseSamples = 0;
	}
}


void KarplusString::Strike(const float f0, const float accent, const float structure, float brightness, float damping,
		const uint32_t strings)
{
	// plaits::StringEngine: each trigger moves to the next string
	const uint32_t count = std::clamp<uint32_t>(strings, 1, maxStrings);
	next = (next + 1) % count;
	Voice& v = voice[next];

	// plaits::StringVoice::Render (per block there, per hit here)
	brightness += 0.25f * accent * (1.0f - brightness);
	damping += 0.25f * accent * (1.0f - damping);

	const float range = 72.0f;
	const float f = 4.0f * f0;
	const float cutoff = std::min(f * stmlib::SemitonesToRatio((brightness * (2.0f - brightness) - 0.5f) * range), 0.499f);
	v.noiseSamples = static_cast<uint32_t>(1.0f / f0);
	v.excitation.set_f_q<stmlib::FREQUENCY_DIRTY>(cutoff, 0.5f);

	const float nonLinearity = structure < 0.24f
		? (structure - 0.24f) * 4.166f
		: (structure > 0.26f ? (structure - 0.26f) * 1.35135f : 0.0f);
	v.string.Configure(f0, nonLinearity, brightness, damping);
	v.quietCount = 0;
	v.active = true;
}


float KarplusString::Process()
{
	float out = 0.0f;
	for (auto& v : voice) {
		if (!v.active) {
			continue;
		}
		float e = 0.0f;
		if (v.noiseSamples) {
			--v.noiseSamples;
			e = 2.0f * stmlib::Random::GetFloat() - 1.0f;
		}
		e = v.excitation.Process<stmlib::FILTER_MODE_LOW_PASS>(e);
		const float s = v.string.Process(e);

		if (!(std::abs(s) < 1000.0f)) {				// nan / inf (never seen): clear this string, stay silent
			v.string.Reset();
			v.excitation.Init();
			v.active = false;
			continue;
		}
		out += s;
		if (std::abs(s) < 1e-5f) {
			if (++v.quietCount > quietLimit) {
				v.active = false;
			}
		} else {
			v.quietCount = 0;
		}
	}
	limiter.Process(1.0f, &out, 1);
	return std::clamp(out, -1.0f, 1.0f);			// Plaits clips its 16-bit output at full scale
}


bool KarplusString::Active() const
{
	for (auto& v : voice) {
		if (v.active) {
			return true;
		}
	}
	return false;
}
