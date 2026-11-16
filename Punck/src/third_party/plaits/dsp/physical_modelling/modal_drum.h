// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Modal synthesis voice with a mallet exciter (click -> LPF -> resonator), after Plaits' ModalVoice /
// Resonator (taken from Rings). Modified for the Punck (modulove fork, same MIT licence):
// - the per-mode filter coefficients are computed once per hit instead of every block, spread over a few samples
//   (kModesPerSample) so a hit never overloads a single sample of the 48 kHz interrupt
// - the stiffness table is computed at start-up from the same formula as plaits/resources/lookup_tables.py
// - the number of modes is selectable (CPU vs. brightness)

#ifndef PLAITS_DSP_PHYSICAL_MODELLING_MODAL_DRUM_H_
#define PLAITS_DSP_PHYSICAL_MODELLING_MODAL_DRUM_H_

#include <algorithm>
#include <cmath>

#include "stmlib/dsp/cosine_oscillator.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/units.h"

namespace plaits {

class ModalDrum {
 public:
  static constexpr int kMaxModes = 24;

  // Start-up only (uses pow/cos): mode amplitudes and stiffness table
  void Init(int modes = kMaxModes) {
    num_modes_ = std::min(modes, kMaxModes);
    // Mode amplitudes for a fixed strike position (Plaits: 0.015)
    stmlib::CosineOscillator amplitudes;
    amplitudes.Init<stmlib::COSINE_OSCILLATOR_APPROXIMATE>(0.015f);
    for (int i = 0; i < kMaxModes; ++i) {
      amplitude_[i] = amplitudes.Next() * 0.25f;
      gain_[i] = 0.0f;
      g_[i] = 0.0f;
      r_plus_g_[i] = 1.0f;
      h_[i] = 1.0f;
      s1_[i] = s2_[i] = 0.0f;
    }
    // Stiffness (partial detuning) table, same formula as Plaits' lookup_tables.py
    for (int i = 0; i <= 64; ++i) {
      float g = i / 64.0f;
      float s;
      if (g < 0.25f) {
        s = -(0.25f - g) * 0.25f;
      } else if (g < 0.3f) {
        s = 0.0f;
      } else if (g < 0.9f) {
        g = (g - 0.3f) / 0.6f;
        s = 0.01f * std::pow(10.0f, g * 2.005f) - 0.01f;
      } else {
        g = (g - 0.9f) / 0.1f;
        g *= g;
        s = 1.5f - std::cos(g * 3.14159265f) / 2.0f;
      }
      stiffness_table_[i] = s;
    }
    stiffness_table_[64] = stiffness_table_[63] = 2.0f;
    stiffness_table_[65] = 2.0f;
    exciter_s1_ = exciter_s2_ = 0.0f;
    pulse_ = 0.0f;
  }

  // Cheap: clears the filter states (safe inside the audio interrupt)
  void Reset() {
    for (int i = 0; i < kMaxModes; ++i) {
      s1_[i] = s2_[i] = 0.0f;
    }
    exciter_s1_ = exciter_s2_ = 0.0f;
    pulse_ = 0.0f;
  }

  // Called once per hit: f0 normalised frequency, structure (inharmonicity 0..1), brightness, damping (0..1)
  void Strike(float f0, float structure, float brightness, float damping, float accent) {
    brightness += 0.25f * accent * (1.0f - brightness);
    damping += 0.25f * accent * (1.0f - damping);

    // Exciter: impulse through a resonant low pass whose cutoff follows the brightness
    const float cutoff = std::min(2.0f * f0 * stmlib::SemitonesToRatio((brightness * (2.0f - brightness) - 0.5f) * 60.0f), 0.499f);
    SetSvf(cutoff, 1.5f, exciter_g_, exciter_rpg_, exciter_h_);
    const float attenuation = 1.0f - damping * 0.5f;
    const float amplitude = (0.12f + 0.08f * accent) * attenuation;
    pulse_ = amplitude * stmlib::SemitonesToRatio(cutoff * cutoff * 24.0f) / cutoff;

    // Resonator modes (Plaits Resonator::Process, coefficient part)
    float stiffness = stmlib::Interpolate(stiffness_table_, structure, 64.0f);
    f0 *= NthHarmonicCompensation(3, stiffness);
    float harmonic = f0;
    float stretch_factor = 1.0f;
    const float q_sqrt = stmlib::SemitonesToRatio(damping * 79.7f);
    float q = 500.0f * q_sqrt * q_sqrt;
    brightness *= 1.0f - structure * 0.3f;
    brightness *= 1.0f - damping * 0.3f;
    // The mode coefficients are computed a few modes per sample in Process (kModesPerSample), so a hit costs no
    // more than an ordinary sample; the strike pulse fires once all modes are set (16 modes: 4 samples, 83 us)
    pending_pulse_ = pulse_;
    pulse_ = 0.0f;
    next_mode_ = 0;
    stage_f0_ = f0;
    stage_harmonic_ = harmonic;
    stage_stretch_ = stretch_factor;
    stage_stiffness_ = stiffness;
    stage_q_ = q;
    stage_q_loss_ = brightness * (2.0f - brightness) * 0.85f + 0.15f;
  }

  // One sample: in = extra excitation (e.g. noise), returns the resonator output
  inline float Process(float in) {
    if (next_mode_ < num_modes_) {
      ConfigureModes();
    }
    // Exciter low pass
    const float e_in = pulse_ + in;
    pulse_ = 0.0f;
    const float ehp = (e_in - exciter_rpg_ * exciter_s1_ - exciter_s2_) * exciter_h_;
    const float ebp = exciter_g_ * ehp + exciter_s1_;
    exciter_s1_ = exciter_g_ * ehp + ebp;
    const float elp = exciter_g_ * ebp + exciter_s2_;
    exciter_s2_ = exciter_g_ * ebp + elp;

    // Band pass mode bank
    float out = 0.0f;
    for (int i = 0; i < num_modes_; ++i) {
      const float hp = (elp - r_plus_g_[i] * s1_[i] - s2_[i]) * h_[i];
      const float bp = g_[i] * hp + s1_[i];
      s1_[i] = g_[i] * hp + bp;
      const float lp = g_[i] * bp + s2_[i];
      s2_[i] = g_[i] * bp + lp;
      out += gain_[i] * bp;
    }
    return out;
  }

  void set_num_modes(int n) { num_modes_ = std::clamp(n, 4, kMaxModes); }

 private:
  static constexpr int kModesPerSample = 4;

  void ConfigureModes() {
    const int end = std::min(next_mode_ + kModesPerSample, num_modes_);
    for (int i = next_mode_; i < end; ++i) {
      const float mode_frequency = std::min(stage_harmonic_ * stage_stretch_, 0.499f);
      const float mode_attenuation = 1.0f - mode_frequency * 2.0f;
      SetSvf(mode_frequency, 1.0f + mode_frequency * stage_q_, g_[i], r_plus_g_[i], h_[i]);
      gain_[i] = amplitude_[i] * mode_attenuation;
      stage_stretch_ += stage_stiffness_;
      stage_stiffness_ *= (stage_stiffness_ < 0.0f) ? 0.93f : 0.98f;
      stage_harmonic_ += stage_f0_;
      stage_q_ *= stage_q_loss_;
    }
    next_mode_ = end;
    if (next_mode_ >= num_modes_) {
      pulse_ = pending_pulse_;					// all modes ready: strike
      pending_pulse_ = 0.0f;
    }
  }

  static void SetSvf(float f, float q, float& g, float& r_plus_g, float& h) {
    g = stmlib::OnePole::tan<stmlib::FREQUENCY_FAST>(f);
    const float r = 1.0f / q;
    h = 1.0f / (1.0f + r * g + g * g);
    r_plus_g = r + g;
  }

  static float NthHarmonicCompensation(int n, float stiffness) {
    float stretch_factor = 1.0f;
    for (int i = 0; i < n - 1; ++i) {
      stretch_factor += stiffness;
      stiffness *= (stiffness < 0.0f) ? 0.93f : 0.98f;
    }
    return 1.0f / stretch_factor;
  }

  int num_modes_ = kMaxModes;
  float amplitude_[kMaxModes];
  float g_[kMaxModes], r_plus_g_[kMaxModes], h_[kMaxModes], gain_[kMaxModes];
  float s1_[kMaxModes], s2_[kMaxModes];
  float stiffness_table_[66];
  float exciter_g_ = 0.1f, exciter_rpg_ = 1.0f, exciter_h_ = 1.0f, exciter_s1_ = 0.0f, exciter_s2_ = 0.0f;
  float pulse_ = 0.0f;
  float pending_pulse_ = 0.0f;
  int next_mode_ = kMaxModes;
  float stage_f0_ = 0.0f, stage_harmonic_ = 0.0f, stage_stretch_ = 1.0f, stage_stiffness_ = 0.0f;
  float stage_q_ = 1.0f, stage_q_loss_ = 1.0f;
};

}  // namespace plaits

#endif  // PLAITS_DSP_PHYSICAL_MODELLING_MODAL_DRUM_H_
