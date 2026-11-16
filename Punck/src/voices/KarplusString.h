#pragma once
#include <cstdint>
#include "plaits/dsp/physical_modelling/string.h"
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/limiter.h"

// Mutable Instruments Plaits string engine (MIT licence, see third_party/LICENSE): Karplus-Strong string with a
// curved bridge (buzz) or dispersion (inharmonic, metallic), struck by a low-passed noise burst one period long
// (plaits/dsp/physical_modelling/string_voice.cc, plaits/dsp/engine/string_engine.cc), then Plaits' output limiter
// (the string engine is registered with out_gain -1: stmlib::Limiter at unity gain; no low-pass gate).
// Adapted for the Punck (modulove): one sample per call; each hit takes the next of up to 3 strings so tails overlap
// as in Plaits; each string keeps the settings of its own hit; quiet strings are skipped (no CPU).
class KarplusString {
public:
	static constexpr uint32_t maxStrings = 3;
	static constexpr uint32_t floatsPerString = plaits::kDelayLineSize + plaits::kDelayLineSize / 4;
	static constexpr uint32_t bufferFloats = maxStrings * floatsPerString;

	void Init(float* buffer);				// Start-up only: clears the delay lines (3 x 5 kB)
	void Silence();							// Engine switch / recovery: strings stop at once (fast, buffers kept)

	// f0: frequency / sample rate. structure 0 - 1 (Plaits HARMONICS: below 0.24 curved bridge / buzz, 0.24 - 0.26
	// clean string, above dispersion: inharmonic, metallic). brightness 0 - 1 (Plaits TIMBRE squared). damping 0 - 1
	// (Plaits MORPH: longer towards 1; 0.95 and above sustains forever, so callers keep it lower).
	void Strike(float f0, float accent, float structure, float brightness, float damping, uint32_t strings);
	float Process();						// One sample: sum of the strings through the limiter (peak ~0.8, clipped at 1)
	bool Active() const;

private:
	static constexpr uint32_t quietLimit = 4800;		// 100 ms below -100 dBFS: the string is skipped until struck

	struct Voice {
		plaits::String string;
		stmlib::Svf excitation;
		uint32_t noiseSamples = 0;
		uint32_t quietCount = 0;
		bool active = false;
	} voice[maxStrings];
	uint32_t next = 0;
	stmlib::Limiter limiter;
};
