#pragma once
#include <cstdint>
#include "stmlib/dsp/filter.h"
#include "plaits/dsp/dsp.h"
#include "plaits/dsp/envelope.h"
#include "plaits/dsp/oscillator/oscillator.h"
#include "plaits/dsp/oscillator/variable_shape_oscillator.h"
#include "plaits/dsp/oscillator/variable_saw_oscillator.h"

// Mutable Instruments Plaits (MIT licence, see third_party/LICENSE): a triggered Plaits voice for the Punck (modulove).
// - Every 12 samples (plaits::kBlockSize) the control part of plaits::Voice::Render: the internal decay envelope on
//   pitch / timbre / morph (what the FM / TIMBRE / MORPH attenuverters do when TRIG is patched) and the LPG vactrol
//   envelope (decay, colour).
// - Per sample the engine, with the parameters Plaits interpolates over each block:
//     FM   = plaits/dsp/engine/fm_engine.cc: 2-op FM with feedback, 4x oversampled (main output)
//     VA   = virtual_analog_engine.cc, variant 2 main output: variable square with hard sync + variable saw
//     Fold = waveshaping_engine.cc main output: slope oscillator -> waveshaper -> wavefolder
// - The low-pass gate (plaits/dsp/fx/low_pass_gate.h) with the engine's output gain, then Plaits' full-scale clip.
// One voice (Plaits is monophonic); quiet after the gate has closed (no CPU).
class PlaitsSynth {
public:
	enum Model : uint8_t {fm = 0, va, fold, modelCount};
	struct Patch {
		float harmonics = 0.5f, timbre = 0.5f, morph = 0.5f;			// Plaits knobs 0 - 1
		float decay = 0.5f, colour = 0.5f;								// LPG decay and colour (response) 0 - 1
		float pitchEnv = 0.0f, timbreEnv = 0.0f, morphEnv = 0.0f;		// Internal envelope amounts -1 .. 1 (attenuverters)
	};

	void Init();
	void Silence();										// Engine switch / nan recovery: stop and reset the engine state
	void Trigger(float hz, float accent, const Patch& p, Model m);		// accent 0 - 1 scales the output (velocity)
	float Process();													// One sample
	bool Active() const { return active; }

private:
	static constexpr uint32_t kBlock = plaits::kBlockSize;			// 12

	struct Ramp {														// stmlib::ParameterInterpolator over one block
		float value = 0.0f, inc = 0.0f;
		bool primed = false;
		void Set(const float target) {
			if (!primed) {												// first block: start at the target (Plaits'
				value = target;											// oscillators start from their own defaults,
				inc = 0.0f;												// a ramp from 0 made the slope's pw jump)
				primed = true;
				return;
			}
			inc = (target - value) * (1.0f / kBlock);
		}
		float Next() { value += inc; return value; }
	};

	static float NoteToF(float note);									// plaits::NoteToFrequency at 48 kHz
	void ControlBlock();
	void BlockFM(float note, float timbre, float morph, float harmonics);
	void BlockVA(float note, float timbre, float morph, float harmonics);
	void BlockFold(float note, float timbre, float morph, float harmonics);
	float NextFM();
	float NextVA();
	float NextFold();

	Model model = fm;
	Patch patch;
	float note = 60.0f;					// MIDI note of the hit (fractional)
	float accent = 1.0f;
	bool active = false;
	uint32_t blockPos = 0;
	uint32_t blocks = 0;				// Blocks since the trigger

	plaits::DecayEnvelope decayEnvelope;
	plaits::LPGEnvelope lpgEnvelope;
	stmlib::Svf lpgFilter;
	Ramp lpgGain;
	float lpgBleed = 0.0f;

	// FM
	uint32_t carrierPhase = 0, modulatorPhase = 0;
	float previousSample = 0.0f, carrierFir = 0.0f;
	Ramp carrierF, modulatorF, fmAmount, fmFeedback;

	// VA
	plaits::VariableShapeOscillator sync;
	plaits::VariableSawOscillator saw;
	Ramp primaryF, squareSyncF, squarePw, auxiliaryF, sawPw, sawShape, squareGain, sawGain;

	// Fold
	plaits::Oscillator slope;
	Ramp slopeF, slopePw, wsShape, wfGain;
};
