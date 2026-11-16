#pragma once
#include <algorithm>
#include <cstdint>
#include "Dynamics.h"

// modulove: Level pot travel (kick, snare, hats, sampler A / B)
//   0 - 75 %   level, audio taper (cubic: -10.6 dB at noon, -5.8 dB at 60 %, unity at 75 % = the old noon level)
//   75 - 77 %  unity plateau (no drive)
//   77 - 100 % drive: saturation blended in and pushed harder towards full CW; the level stays at unity
namespace levelpot {
	constexpr float unityPos = 0.75f;
	constexpr float driveStart = 0.77f;

	inline float Gain(const uint16_t adc)
	{
		const float x = static_cast<float>(adc) * (1.0f / 65535.0f);
		if (x >= unityPos) {
			return 1.0f;
		}
		const float t = x * (1.0f / unityPos);
		return t * t * t;
	}

	inline float Drive(const uint16_t adc)						// 0 - 1
	{
		const float x = static_cast<float>(adc) * (1.0f / 65535.0f);
		return x <= driveStart ? 0.0f : std::min((x - driveStart) * (1.0f / (1.0f - driveStart)), 1.0f);
	}

	// Drive parameters for a pot position (main loop): amount 0 - 1, input gain, output norm, dry/driven crossfade
	struct DriveParams {
		float amount = 0.0f, gain = 1.0f, norm = 1.0f, fade = 0.0f;
	};
	inline DriveParams Params(const uint16_t adc, const float maxDb)
	{
		DriveParams d;
		d.amount = Drive(adc);
		d.gain = dyn::FastExp2(d.amount * std::clamp(maxDb, 0.0f, 24.0f) * (1.0f / 6.0206f));
		const float s = std::min(d.gain, 1.5f);
		d.norm = 1.0f / (s - (4.0f / 27.0f) * s * s * s);		// soft clip of the gain itself: full scale stays full scale
		d.fade = std::min(4.0f * d.amount, 1.0f);				// dry -> driven over the first quarter of the zone
		return d;
	}

	// Per-voice drive stage (audio interrupt): anti-aliased soft clip (dyn::Clipper warm), crossfaded with the dry signal
	struct DriveStage {
		dyn::Clipper clip[2];
		bool active = false;
		void Process(float& l, float& r, const float gain, const float norm, const float fade)
		{
			if (!active) {											// no step from a stale anti-aliasing state
				clip[0].Prime(l * gain);
				clip[1].Prime(r * gain);
				active = true;
			}
			l += fade * (clip[0].Process(l * gain, dyn::Clipper::warm) * norm - l);
			r += fade * (clip[1].Process(r * gain, dyn::Clipper::warm) * norm - r);
		}
	};
}
