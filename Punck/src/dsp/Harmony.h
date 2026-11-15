#pragma once
// modulove: key & scale for the tonal voices - scales as 12 bit pitch class masks, nearest-note quantizer
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <initializer_list>

namespace harmony {

// bit 0 = root ... bit 11 = major 7th
constexpr uint16_t ScaleMask(std::initializer_list<uint8_t> intervals)
{
	uint16_t mask = 0;
	for (const uint8_t i : intervals) {
		mask |= static_cast<uint16_t>(1u << (i % 12));
	}
	return mask;
}

constexpr uint8_t scaleCount = 11;
constexpr uint16_t scaleMasks[scaleCount] = {
	ScaleMask({0, 2, 3, 5, 7, 8, 10}),		// 0 minor (Aeolian)
	ScaleMask({0, 2, 3, 5, 7, 9, 10}),		// 1 Dorian
	ScaleMask({0, 1, 3, 5, 7, 8, 10}),		// 2 Phrygian
	ScaleMask({0, 1, 4, 5, 7, 8, 10}),		// 3 Phrygian dominant
	ScaleMask({0, 2, 3, 5, 7, 8, 11}),		// 4 harmonic minor
	ScaleMask({0, 3, 5, 7, 10}),			// 5 minor pentatonic
	ScaleMask({0, 2, 4, 5, 7, 9, 11}),		// 6 major (Ionian)
	ScaleMask({0, 2, 4, 5, 7, 9, 10}),		// 7 Mixolydian
	ScaleMask({0, 2, 4, 6, 7, 9, 11}),		// 8 Lydian
	ScaleMask({0, 2, 4, 7, 9}),				// 9 major pentatonic
	ScaleMask({0, 3, 5, 10}),				// 10 quartal
};
constexpr uint16_t minPentatonic = ScaleMask({0, 3, 5, 7, 10});
constexpr uint16_t majPentatonic = ScaleMask({0, 2, 4, 7, 9});

// Voice tune mode -> allowed pitch classes: 0 off, 1 root, 2 root + fifth, 3 scale, 4 pentatonic notes of the scale
inline uint16_t ModeMask(const uint32_t mode, const uint32_t scaleIndex)
{
	const uint16_t scale = scaleMasks[std::min(scaleIndex, static_cast<uint32_t>(scaleCount - 1))];
	switch (mode) {
		case 1:  return 0x001;
		case 2:  return 0x081;
		case 3:  return scale;
		case 4: {
			const uint16_t sub = scale & ((scale & (1u << 3)) ? minPentatonic : majPentatonic);
			return (__builtin_popcount(sub) >= 3) ? sub : 0x081;
		}
		default: return 0;
	}
}

// Nearest allowed note within +/- 6 semitones of a fractional MIDI note; equal distances prefer the root, then the fifth
inline float QuantizeNote(const float note, const int32_t root, const uint16_t mask)
{
	const int32_t n0 = static_cast<int32_t>(std::floor(note + 0.5f));
	float best = note, bestDist = 1e9f;
	for (int32_t n = n0 - 6; n <= n0 + 6; ++n) {
		const int32_t pc = ((n - root) % 12 + 12) % 12;
		if (mask & (1u << pc)) {
			const float dist = std::abs(static_cast<float>(n) - note) - (pc == 0 ? 0.02f : pc == 7 ? 0.01f : 0.0f);
			if (dist < bestDist) {
				bestDist = dist;
				best = static_cast<float>(n);
			}
		}
	}
	return best;
}

}		// namespace harmony
