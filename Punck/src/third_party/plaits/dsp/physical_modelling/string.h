// Copyright 2016 Emilie Gillet.
// modulove (Punck fork, same MIT licence): one sample per call - the per-block setup of String::Process runs in
// Configure() only when a parameter changes, Process() is the body of the original per-sample loop.
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
// Comb filter / KS string. "Lite" version of the implementation used in Rings.


#ifndef PLAITS_DSP_PHYSICAL_MODELLING_STRING_H_
#define PLAITS_DSP_PHYSICAL_MODELLING_STRING_H_

#include <algorithm>
#include <cmath>

#include "stmlib/stmlib.h"

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/units.h"
#include "stmlib/utils/random.h"

#include "plaits/dsp/dsp.h"
#include "plaits/dsp/physical_modelling/delay_line.h"
#include "plaits/resources.h"

namespace plaits {

const size_t kDelayLineSize = 1024;

class String {
 public:
  String() { }
  ~String() { }

  // modulove: buffers come from the caller (kDelayLineSize and kDelayLineSize / 4 floats)
  void Init(float* string_buffer, float* stretch_buffer) {
    string_.Init(string_buffer);
    stretch_.Init(stretch_buffer);
    delay_ = 100.0f;
    Reset();
  }

  void Reset() {
    string_.Reset();
    stretch_.Reset();
    iir_damping_filter_.Init();
    dc_blocker_.Init(1.0f - 20.0f / kSampleRate);
    dispersion_noise_ = 0.0f;
    curved_bridge_ = 0.0f;
    out_sample_[0] = out_sample_[1] = 0.0f;
    src_phase_ = 0.0f;
    cfg_f0_ = -1.0f;
  }

  // Per-block part of the original String::Process (non_linearity_amount < 0: curved bridge, > 0: dispersion)
  void Configure(float f0, float non_linearity_amount, float brightness, float damping) {
    if (f0 == cfg_f0_ && non_linearity_amount == cfg_nl_ && brightness == cfg_brightness_ && damping == cfg_damping_) {
      return;
    }
    cfg_f0_ = f0;
    cfg_nl_ = non_linearity_amount;
    cfg_brightness_ = brightness;
    cfg_damping_ = damping;

    dispersion_ = non_linearity_amount > 0.0f;
    if (!dispersion_) {
      non_linearity_amount = -non_linearity_amount;
    }

    float delay = 1.0f / f0;
    CONSTRAIN(delay, 4.0f, kDelayLineSize - 4.0f);

    // If there is not enough delay time in the delay line, we play at the
    // lowest possible note and we upsample on the fly with a shitty linear
    // interpolator. We don't care because it's a corner case (f0 < 11.7Hz)
    float src_ratio = delay * f0;
    if (src_ratio >= 0.9999f) {
      // When we are above 11.7 Hz, we make sure that the linear interpolator
      // does not get in the way.
      src_phase_ = 1.0f;
      src_ratio = 1.0f;
    }
    src_ratio_ = src_ratio;

    float damping_cutoff = std::min(
        12.0f + damping * damping * 60.0f + brightness * 24.0f,
        84.0f);
    float damping_f = std::min(f0 * stmlib::SemitonesToRatio(damping_cutoff), 0.499f);

    // Crossfade to infinite decay.
    if (damping >= 0.95f) {
      float to_infinite = 20.0f * (damping - 0.95f);
      brightness += to_infinite * (1.0f - brightness);
      damping_f += to_infinite * (0.4999f - damping_f);
      damping_cutoff += to_infinite * (128.0f - damping_cutoff);
    }

    iir_damping_filter_.set_f_q<stmlib::FREQUENCY_FAST>(damping_f, 0.5f);

    float damping_compensation = stmlib::Interpolate(lut_svf_shift, damping_cutoff, 1.0f);

    // Original: delay time linearly interpolated over the block (the Punck renders one sample per call)
    delay_ = delay * damping_compensation;

    stretch_point_ = non_linearity_amount * (2.0f - non_linearity_amount) * 0.225f;
    float stretch_correction = (160.0f / kSampleRate) * delay;
    CONSTRAIN(stretch_correction, 1.0f, 2.1f);
    stretch_correction_ = stretch_correction;

    float noise_amount_sqrt = non_linearity_amount > 0.75f
        ? 4.0f * (non_linearity_amount - 0.75f)
        : 0.0f;
    noise_amount_ = noise_amount_sqrt * noise_amount_sqrt * 0.1f;
    noise_filter_ = 0.06f + 0.94f * brightness * brightness;

    float bridge_curving_sqrt = non_linearity_amount;
    bridge_curving_ = bridge_curving_sqrt * bridge_curving_sqrt * 0.01f;

    ap_gain_ = -0.618f * non_linearity_amount / (0.15f + fabsf(non_linearity_amount));
  }

  // One sample: body of the original per-sample loop
  inline float Process(float in) {
    src_phase_ += src_ratio_;
    if (src_phase_ > 1.0f) {
      src_phase_ -= 1.0f;

      float delay = delay_;
      float s = 0.0f;

      if (dispersion_) {
        float noise = stmlib::Random::GetFloat() - 0.5f;
        ONE_POLE(dispersion_noise_, noise, noise_filter_)
        delay *= 1.0f + dispersion_noise_ * noise_amount_;
        float ap_delay = delay * stretch_point_;
        float main_delay = delay - ap_delay * (0.408f - stretch_point_ * 0.308f) * stretch_correction_;
        if (ap_delay >= 4.0f && main_delay >= 4.0f) {
          s = string_.Read(main_delay);
          s = stretch_.Allpass(s, static_cast<size_t>(ap_delay), ap_gain_);
        } else {
          s = string_.ReadHermite(delay);
        }
      } else {
        delay *= 1.0f - curved_bridge_ * bridge_curving_;
        s = string_.ReadHermite(delay);
        float value = fabsf(s) - 0.025f;
        float sign = s > 0.0f ? 1.0f : -1.5f;
        curved_bridge_ = (fabsf(value) + value) * sign;
      }

      s += in;
      CONSTRAIN(s, -20.0f, +20.0f);

      dc_blocker_.Process(&s, 1);
      s = iir_damping_filter_.Process<stmlib::FILTER_MODE_LOW_PASS>(s);
      string_.Write(s);

      out_sample_[1] = out_sample_[0];
      out_sample_[0] = s;
    }
    return stmlib::Crossfade(out_sample_[1], out_sample_[0], src_phase_);
  }

 private:
  DelayLine<float, kDelayLineSize> string_;
  DelayLine<float, kDelayLineSize / 4> stretch_;

  stmlib::Svf iir_damping_filter_;
  stmlib::DCBlocker dc_blocker_;

  float delay_;
  float dispersion_noise_;
  float curved_bridge_;

  // Very crappy linear interpolation upsampler used for low pitches that
  // do not fit the delay line. Rarely used.
  float src_phase_;
  float out_sample_[2];

  // modulove: values the original computes per block
  bool dispersion_ = false;
  float src_ratio_ = 1.0f, stretch_point_ = 0.0f, stretch_correction_ = 1.0f, noise_amount_ = 0.0f;
  float noise_filter_ = 0.0f, bridge_curving_ = 0.0f, ap_gain_ = 0.0f;
  float cfg_f0_ = -1.0f, cfg_nl_ = 0.0f, cfg_brightness_ = 0.0f, cfg_damping_ = 0.0f;

  DISALLOW_COPY_AND_ASSIGN(String);
};

}  // namespace plaits

#endif  // PLAITS_DSP_PHYSICAL_MODELLING_STRING_H_
