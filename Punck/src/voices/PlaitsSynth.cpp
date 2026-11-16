#include "PlaitsSynth.h"
#include <algorithm>
#include <cmath>
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"
#include "plaits/resources.h"

using namespace stmlib;

// Audio interrupt code into ITCM (the rest of this file runs from flash)
#define PLAITS_ITCM __attribute__((section(".itcm_text.plaits")))

namespace {
	constexpr float outGains[PlaitsSynth::modelCount] = {0.6f, 0.8f, 0.7f};	// plaits::Voice::Init RegisterInstance out_gain

	// plaits::Voice::ApplyModulations: attenuverter response
	inline float ModulationAmount(float a)
	{
		a *= std::max(std::fabs(a) - 0.05f, 0.05f);
		return a * 1.05f;
	}

	// plaits::VirtualAnalogEngine::ComputeDetuning
	const float intervals[5] = {0.0f, 7.01f, 12.01f, 19.01f, 24.01f};
	inline float Squash(float x) { return x * x * (3.0f - 2.0f * x); }
	PLAITS_ITCM float ComputeDetuning(float detune)
	{
		detune = 2.05f * detune - 1.025f;
		CONSTRAIN(detune, -1.0f, 1.0f);
		const float sign = detune < 0.0f ? -1.0f : 1.0f;
		detune = detune * sign * 3.9999f;
		MAKE_INTEGRAL_FRACTIONAL(detune);
		const float a = intervals[detune_integral];
		const float b = intervals[detune_integral + 1];
		return (a + (b - a) * Squash(Squash(detune_fractional))) * sign;
	}

	// plaits::WaveshapingEngine Tame
	PLAITS_ITCM float Tame(float f0, const float harmonics, const float order)
	{
		f0 *= harmonics;
		const float max_f = 0.5f / order;
		float max_amount = 1.0f - (f0 - max_f) / (0.5f - max_f);
		CONSTRAIN(max_amount, 0.0f, 1.0f);
		return max_amount * max_amount * max_amount;
	}
}


PLAITS_ITCM float PlaitsSynth::NoteToF(float n)
{
	// plaits::NoteToFrequency with the Punck's exact 48 kHz (Plaits itself runs at 47.872 kHz)
	n -= 9.0f;
	CONSTRAIN(n, -128.0f, 127.0f);
	return (440.0f / 32.0f) / plaits::kSampleRate * SemitonesToRatio(n);
}


void PlaitsSynth::Init()
{
	decayEnvelope.Init();
	lpgEnvelope.Init();
	lpgFilter.Init();
	sync.Init();
	saw.Init();
	slope.Init();
	carrierPhase = modulatorPhase = 0;
	previousSample = carrierFir = 0.0f;
	active = false;
}


void PlaitsSynth::Silence()
{
	Init();									// cheap (no buffers): also clears any nan from the oscillators
}


PLAITS_ITCM void PlaitsSynth::Trigger(const float hz, const float accent, const Patch& p, const Model m)
{
	model = m < modelCount ? m : fm;
	patch = p;
	this->accent = accent;
	note = 69.0f + 12.0f * std::log2(std::max(hz, 1.0f) / 440.0f);
	lpgEnvelope.Trigger();
	decayEnvelope.Trigger();
	blockPos = 0;
	blocks = 0;
	active = true;
}


PLAITS_ITCM void PlaitsSynth::ControlBlock()
{
	// plaits::Voice::Render, per block (TRIG patched, LEVEL unpatched)
	const float shortDecay = (200.0f * kBlock) / plaits::kSampleRate * SemitonesToRatio(-96.0f * patch.decay);
	decayEnvelope.Process(shortDecay * 2.0f);
	const float env = decayEnvelope.value();

	const float n = std::clamp(note + ModulationAmount(patch.pitchEnv) * env * env * 48.0f, -119.0f, 120.0f);
	const float t = std::clamp(patch.timbre + ModulationAmount(patch.timbreEnv) * env, 0.0f, 1.0f);
	const float m = std::clamp(patch.morph + ModulationAmount(patch.morphEnv) * env, 0.0f, 1.0f);
	const float h = std::clamp(patch.harmonics, 0.0f, 1.0f);
	switch (model) {
		case fm:	BlockFM(n, t, m, h);	break;
		case va:	BlockVA(n, t, m, h);	break;
		default:	BlockFold(n, t, m, h);	break;
	}

	// LPG
	const float hf = std::clamp(patch.colour, 0.0f, 1.0f);
	const float decayTail = (20.0f * kBlock) / plaits::kSampleRate * SemitonesToRatio(-72.0f * patch.decay + 12.0f * hf) - shortDecay;
	const float attack = NoteToF(n) * static_cast<float>(kBlock) * 2.0f;
	lpgEnvelope.ProcessPing(attack, shortDecay, decayTail, hf);
	lpgGain.Set(outGains[model] * accent * lpgEnvelope.gain());
	lpgFilter.set_f_q<FREQUENCY_DIRTY>(lpgEnvelope.frequency(), 0.4f);
	lpgBleed = lpgEnvelope.hf_bleed();
	++blocks;
}


PLAITS_ITCM float PlaitsSynth::Process()
{
	if (!active) {
		return 0.0f;
	}
	if (blockPos == 0) {
		ControlBlock();
	}
	if (++blockPos >= kBlock) {
		blockPos = 0;
		if (blocks > 4 && lpgEnvelope.gain() < 1e-4f) {		// gate closed (-80 dB): stop after this block
			active = false;
		}
	}

	float s;
	switch (model) {
		case fm:	s = NextFM();	break;
		case va:	s = NextVA();	break;
		default:	s = NextFold();	break;
	}

	// plaits::LowPassGate::Process
	s *= lpgGain.Next();
	const float lp = lpgFilter.Process<FILTER_MODE_LOW_PASS>(s);
	s = lp + (s - lp) * lpgBleed;
	return std::clamp(s, -1.0f, 1.0f);					// Plaits clips its 16-bit output at full scale
}


// ------------------------------------------------------------------------------------------------------------------
// FM (plaits::FMEngine::Render)
PLAITS_ITCM void PlaitsSynth::BlockFM(const float n, const float t, const float m, const float h)
{
	const float noteC = n - 24.0f;										// 4x oversampling
	const float ratio = Interpolate(plaits::lut_fm_frequency_quantizer, h, 128.0f);
	const float modulatorNote = noteC + ratio;
	float targetModulatorF = NoteToF(modulatorNote);
	CONSTRAIN(targetModulatorF, 0.0f, 0.5f);

	// Reduce the maximum FM index for high pitched notes, to prevent aliasing.
	float hfTaming = 1.0f - (modulatorNote - 72.0f) * 0.025f;
	CONSTRAIN(hfTaming, 0.0f, 1.0f);
	hfTaming *= hfTaming;

	carrierF.Set(NoteToF(noteC));
	modulatorF.Set(targetModulatorF);
	fmAmount.Set(2.0f * t * t * hfTaming);
	fmFeedback.Set(2.0f * m - 1.0f);
}


PLAITS_ITCM float PlaitsSynth::NextFM()
{
	const float max_uint32 = 4294967296.0f;
	const float amount = fmAmount.Next();
	const float feedback = fmFeedback.Next();
	const float phaseFeedback = feedback < 0.0f ? 0.5f * feedback * feedback : 0.0f;
	const uint32_t carrierIncrement = static_cast<uint32_t>(max_uint32 * carrierF.Next());
	const float mf = modulatorF.Next();
	const float modulatorFb = feedback > 0.0f ? 0.25f * feedback * feedback : 0.0f;

	// plaits::Downsampler (state = the head of the next output sample)
	float head = carrierFir, tail = 0.0f;
	for (uint32_t j = 0; j < 4; ++j) {
		modulatorPhase += static_cast<uint32_t>(max_uint32 * mf * (1.0f + previousSample * phaseFeedback));
		carrierPhase += carrierIncrement;
		const float modulator = plaits::SinePM(modulatorPhase, modulatorFb * previousSample);
		const float carrier = plaits::SinePM(carrierPhase, amount * modulator);
		ONE_POLE(previousSample, carrier, 0.05f);
		head += carrier * plaits::lut_4x_downsampler_fir[3 - j];
		tail += carrier * plaits::lut_4x_downsampler_fir[j];
	}
	carrierFir = tail;
	return head;
}


// ------------------------------------------------------------------------------------------------------------------
// VA (plaits::VirtualAnalogEngine::Render, VA_VARIANT 2, OUT)
PLAITS_ITCM void PlaitsSynth::BlockVA(const float n, const float t, const float m, const float h)
{
	const float auxiliaryDetune = ComputeDetuning(h);
	const float primary = NoteToF(n);
	const float auxiliary = NoteToF(n + auxiliaryDetune);

	float sqPw = 1.3f * t - 0.15f;
	CONSTRAIN(sqPw, 0.005f, 0.5f);
	const float sqSyncRatio = t < 0.5f ? 0.0f : (t - 0.5f) * (t - 0.5f) * 4.0f * 48.0f;
	const float sqGain = std::min(t * 8.0f, 1.0f);

	float swPw = m < 0.5f ? m + 0.5f : 1.0f - (m - 0.5f) * 2.0f;
	swPw *= 1.1f;
	CONSTRAIN(swPw, 0.005f, 1.0f);
	float swShape = 10.0f - 21.0f * m;
	CONSTRAIN(swShape, 0.0f, 1.0f);
	float swGain = 8.0f * (1.0f - m);
	CONSTRAIN(swGain, 0.02f, 1.0f);

	const float norm = 1.0f / std::max(sqGain, swGain);
	primaryF.Set(primary);
	squareSyncF.Set(NoteToF(n + sqSyncRatio));
	squarePw.Set(sqPw);
	auxiliaryF.Set(auxiliary);
	sawPw.Set(swPw);
	sawShape.Set(swShape);
	squareGain.Set(sqGain * 0.3f * norm);
	sawGain.Set(swGain * 0.5f * norm);
}


PLAITS_ITCM float PlaitsSynth::NextVA()
{
	float sq, sw;
	sync.Render(primaryF.Next(), squareSyncF.Next(), squarePw.Next(), 1.0f, &sq, 1);
	saw.Render(auxiliaryF.Next(), sawPw.Next(), sawShape.Next(), &sw, 1);
	return sw * sawGain.Next() + squareGain.Next() * sq;
}


// ------------------------------------------------------------------------------------------------------------------
// Fold (plaits::WaveshapingEngine::Render, OUT)
PLAITS_ITCM void PlaitsSynth::BlockFold(const float n, const float t, const float m, const float h)
{
	const float f0 = NoteToF(n);
	const float pw = m * 0.45f + 0.5f;
	const float slopeAmount = 3.0f + std::fabs(m - 0.5f) * 5.0f;
	const float shapeAmount = std::fabs(h - 0.5f) * 2.0f;
	const float shapeAttenuation = Tame(f0, slopeAmount, 16.0f);
	const float wfAttenuation = Tame(f0, slopeAmount * (3.0f + shapeAmount * shapeAttenuation * 5.0f), 12.0f);
	slopeF.Set(f0);
	slopePw.Set(pw);
	wsShape.Set(0.5f + (h - 0.5f) * shapeAttenuation);
	wfGain.Set(0.03f + 0.46f * t * wfAttenuation);
}


PLAITS_ITCM float PlaitsSynth::NextFold()
{
	float x;
	slope.Render<plaits::OSCILLATOR_SHAPE_SLOPE>(slopeF.Next(), slopePw.Next(), &x, 1);
	if (!(std::fabs(x) <= 4.0f)) {			// safety: keeps the table index in range (Plaits' overshoot wraps as before)
		x = 0.0f;
	}

	float shape = wsShape.Next() * 3.9999f;
	MAKE_INTEGRAL_FRACTIONAL(shape);
	const int16_t* shape_1 = plaits::lookup_table_i16_table[shape_integral];
	const int16_t* shape_2 = plaits::lookup_table_i16_table[shape_integral + 1];

	float ws_index = 127.0f * x + 128.0f;
	MAKE_INTEGRAL_FRACTIONAL(ws_index)
	ws_index_integral &= 255;

	const float x0 = static_cast<float>(shape_1[ws_index_integral]) / 32768.0f;
	const float x1 = static_cast<float>(shape_1[ws_index_integral + 1]) / 32768.0f;
	const float xs = x0 + (x1 - x0) * ws_index_fractional;
	const float y0 = static_cast<float>(shape_2[ws_index_integral]) / 32768.0f;
	const float y1 = static_cast<float>(shape_2[ws_index_integral + 1]) / 32768.0f;
	const float ys = y0 + (y1 - y0) * ws_index_fractional;
	const float mix = xs + (ys - xs) * shape_fractional;
	const float index = mix * wfGain.Next() + 0.5f;
	return InterpolateHermite(plaits::lut_fold + 1, index, 512.0f);
}
