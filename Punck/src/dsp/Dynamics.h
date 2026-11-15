#pragma once
// modulove: master bus dynamics - glue compressor and anti-aliased clipper (own code, MIT like the rest of the fork's additions)
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace dyn {

static constexpr float dbPerBit = 6.0206f;					// 20 * log10(2): the compressor works in log2 units

// Fast log2 (max error ~0.009, ie 0.05 dB) - Laurent de Soras, musicdsp.org
inline float FastLog2(float x)
{
	union { float f; int32_t i; } v {x};
	const int32_t e = ((v.i >> 23) & 255) - 128;
	v.i &= ~(255 << 23);
	v.i += 127 << 23;
	return ((-1.0f / 3.0f) * v.f + 2.0f) * v.f - 2.0f / 3.0f + static_cast<float>(e);
}

// Fast 2^p (relative error ~1e-4) - Paul Mineiro, fastapprox
inline float FastExp2(float p)
{
	const float clipp = std::max(p, -126.0f);
	const float offset = (clipp < 0.0f) ? 1.0f : 0.0f;
	const int32_t w = static_cast<int32_t>(clipp);
	const float z = clipp - static_cast<float>(w) + offset;
	union { uint32_t i; float f; } v {static_cast<uint32_t>((1 << 23) * (clipp + 121.2740575f + 27.7280233f / (4.84252568f - z) - 1.49012907f * z))};
	return v.f;
}


// Stereo linked feed-forward compressor: sidechain high-pass, peak detector, soft knee gain computer,
// attack/release smoothing in the log domain (Giannoulis, Massberg, Reiss 2012 'smooth decoupled' topology)
class Compressor {
public:
	void Configure(const float sampleRate, const float thresholdDb, const float ratio, const float kneeDb,
			const float attackMs, const float releaseMs, const float makeupDb, const float scHpfHz)
	{
		threshold = thresholdDb / dbPerBit;
		slope = 1.0f / std::max(ratio, 1.0f) - 1.0f;
		knee = std::max(kneeDb, 0.0f) / dbPerBit;
		makeup = makeupDb / dbPerBit;
		attackCoef = std::exp(-1.0f / (std::max(attackMs, 0.05f) * 0.001f * sampleRate));
		releaseCoef = std::exp(-1.0f / (std::max(releaseMs, 1.0f) * 0.001f * sampleRate));
		hpCoef = std::exp(-6.2831853f * std::clamp(scHpfHz, 0.0f, 1000.0f) / sampleRate);
		peakCoef = std::exp(-1.0f / (0.008f * sampleRate));			// 8 ms detector release irons out bass ripple
	}

	// Returns the linear gain (incl. make-up) for the current sample
	float Gain(const float l, const float r)
	{
		hpL = hpCoef * (hpL + l - lastL);
		hpR = hpCoef * (hpR + r - lastR);
		lastL = l;
		lastR = r;
		const float level = std::max(std::abs(hpL), std::abs(hpR));
		peak = std::max(level, peak * peakCoef);

		const float x = FastLog2(std::max(peak, 1e-6f));
		const float over = x - threshold;
		float target = 0.0f;											// gain change in log2 units (<= 0)
		if (knee > 0.0f && 2.0f * std::abs(over) <= knee) {
			const float t = over + 0.5f * knee;
			target = slope * t * t / (2.0f * knee);
		} else if (over > 0.0f) {
			target = slope * over;
		}

		const float coef = (target < gain) ? attackCoef : releaseCoef;
		gain = coef * gain + (1.0f - coef) * target;
		return FastExp2(gain + makeup);
	}

	float ReductionDb() const { return -gain * dbPerBit; }

private:
	float threshold = -2.0f, slope = -0.66f, knee = 1.0f, makeup = 0.0f;
	float attackCoef = 0.99f, releaseCoef = 0.999f, hpCoef = 0.99f, peakCoef = 0.997f;
	float hpL = 0.0f, hpR = 0.0f, lastL = 0.0f, lastR = 0.0f;
	float peak = 0.0f;
	float gain = 0.0f;
};


// Clipper with first order antiderivative anti-aliasing (Parker, Zavalishin, Le Bivic 2016).
// Soft: cubic x - 4/27 x^3, unity slope at 0, reaches 1.0 at x = 1.5. Hard: clamp at +/-1.
class Clipper {
public:
	float Process(const float x, const bool hard)
	{
		const float d = x - x1;
		float y;
		if (std::abs(d) > 1e-5f) {
			y = hard ? (HardF(x) - HardF(x1)) / d : (SoftF(x) - SoftF(x1)) / d;
		} else {
			const float m = 0.5f * (x + x1);
			y = hard ? Hard(m) : Soft(m);
		}
		x1 = x;
		return y;
	}

private:
	float x1 = 0.0f;

	static float Soft(const float x)
	{
		if (x > 1.5f) return 1.0f;
		if (x < -1.5f) return -1.0f;
		return x - (4.0f / 27.0f) * x * x * x;
	}
	static float SoftF(const float x)
	{
		const float a = std::abs(x);
		if (a > 1.5f) return a - 0.5625f;
		const float x2 = x * x;
		return 0.5f * x2 - x2 * x2 / 27.0f;
	}
	static float Hard(const float x)
	{
		return std::clamp(x, -1.0f, 1.0f);
	}
	static float HardF(const float x)
	{
		const float a = std::abs(x);
		return a > 1.0f ? a - 0.5f : 0.5f * x * x;
	}
};

}		// namespace dyn
