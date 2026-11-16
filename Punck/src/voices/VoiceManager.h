#pragma once

#include "initialisation.h"
#include "MidiHandler.h"
#include "Samples.h"
#include "Kick.h"
#include "Snare.h"
#include "HiHat.h"
#include "Toms.h"
#include "Claps.h"
#include "Dynamics.h"
#include "Harmony.h"
#include <cstring>

struct NoteMapper {
	enum  TriggerType: uint8_t {noTrigger = 0, triggerBtn = 1, trigger1 = 2, trigger2 = 4};
	DrumVoice* drumVoice = nullptr;
	uint8_t voice;
	uint8_t voiceIndex = 0;				// For use with players that have multiple outputs (eg sampler)
	uint8_t midiLow;
	uint8_t midiHigh;
	uint8_t pitchChannel = 0;			// MIDI channel (1-16) on which the voice is played chromatically (0 = off)
	int16_t pendingPlay = -1;			// hit from a trigger input / button waiting for its turn (CheckButtons: one hit set up per sample)

	struct Trigger {
		GPIO_TypeDef* btnGpioBank;
		uint8_t btnGpioPin;
		GPIO_TypeDef* trgGpioBank;
		uint8_t trgGpioPin;
		GPIO_TypeDef* tr2GpioBank;
		uint8_t tr2GpioPin;

		uint8_t debounce = 0;
		bool buttonOn;

		bool ButtonHeld() {									// direct register read (main loop / console)
			return btnGpioBank && (btnGpioBank->IDR & (1 << btnGpioPin)) == 0;
		}

		// Audio interrupt: inputs come from a per-sample snapshot of the GPIO input registers (idr[port], A = 0 .. K = 10).
		// One read per port instead of one per input: each GPIO register read costs ~40 CPU cycles from the interrupt.
		static uint32_t Port(const GPIO_TypeDef* g) { return (reinterpret_cast<uintptr_t>(g) - GPIOA_BASE) >> 10; }
		static bool Low(const uint32_t* idr, const GPIO_TypeDef* g, const uint8_t pin) {
			return g != nullptr && (idr[Port(g)] & (1UL << pin)) == 0;
		}
		bool ButtonHeld(const uint32_t* idr) const { return Low(idr, btnGpioBank, btnGpioPin); }

		TriggerType IsOn(const uint32_t* idr)  {
			// Priority as upstream: button, then trigger 1, then trigger 2
			const TriggerType triggers = Low(idr, btnGpioBank, btnGpioPin) ? triggerBtn :
										 Low(idr, trgGpioBank, trgGpioPin) ? trigger1 :
										 Low(idr, tr2GpioBank, tr2GpioPin) ? trigger2 : noTrigger;
			if (triggers) {		// Fixme - probably need different debounce on each trigger
				if (debounce == 0) {
					debounce = 20;
					return triggers;
				}
				debounce = 20;
			}
			if (debounce > 0) {
				--debounce;
			}
			return noTrigger;
		}
	} trigger;

	struct PWMLED {
		volatile uint32_t* timerChannel;
		uint32_t minLevel = 0;
		uint32_t written = 0xFFFFFFFF;			// modulove: last value written to the timer (unchanged values are not rewritten)

		uint32_t GetLevel()  {
			return *timerChannel;
		}
		void Level(float brightness)  {
			// modulove: every voice sets its LED every sample; a peripheral write costs bus cycles in the audio interrupt
			const uint32_t v = std::max(minLevel, static_cast<uint32_t>(brightness * 4095.0f));
			if (v != written) {
				*timerChannel = v;
				written = v;
			}
		}
		void setMinLevel(uint32_t level) {		// Sets LED current/minimum brightness: to display currently playing sequence with dimly illuminated button
			minLevel = level;
			*timerChannel = level;
			written = level;
		}
	} pwmLed;
};


// DAC level of 1.0 (upstream chose 0.92 as the maximum before the output stage distorts - verified with tools/level_test.py)
static constexpr float adjOutputScaleValue = 0.92f;
static constexpr float maxOutTrimDb = 0.70f;					// outTrim above 0 dB: clipper ceiling up to 99.7 % of DAC full scale

class VoiceManager {
	friend class CDCHandler;
public:
	enum Voice {kick, snare, hihat, samplerA, samplerB, toms, claps, count};

	VoiceManager();
	void VoiceLED(Voice v, bool on);
	void NoteOn(MidiHandler::MidiNote midiNote, const uint8_t channel);	// channel 0-15
	void KickTriggered(const float velocity);					// Starts sidechain ducking
	void MidiClock();											// MIDI clock tick (24 ppqn)
	float SixteenthSamples();									// Tempo for synced effects: MIDI clock > sequencer > kick interval
	void Output();
	int32_t nextOut[2] = {};								// frame computed by Output, written to the I2S at the next interrupt
	void CheckButtons();
	void StartOneHit();
	void IdleTasks();

	// modulove: nan / inf protection - per voice (Voice order), then reverb, master bus, output
	static constexpr uint32_t nanReverb = Voice::count, nanBus = Voice::count + 1, nanOut = Voice::count + 2;
	uint32_t nanEvents[Voice::count + 3] = {};
	volatile bool reverbClearPending = false;
	static bool Finite(const float x) {
		uint32_t b;
		memcpy(&b, &x, sizeof(b));
		return (b & 0x7F800000u) != 0x7F800000u;				// exponent all ones = inf or nan
	}
	void RecoverVoice(const uint32_t v);
	void SetAllLeds(float val);
	void RestoreAllLeds();


	uint32_t GetConfig(uint8_t** buff);							// Return a pointer to MIDI map config data for saving
	void StoreConfig(const uint8_t* buff, const uint32_t len);	// Reads MIDI map config data back into member values
	uint32_t GetMixConfig(uint8_t** buff);
	void StoreMixConfig(const uint8_t* buff, const uint32_t len);

	Kick kickPlayer;
	Snare snarePlayer;
	Samples samples;
	HiHat hihatPlayer;
	Toms tomsPlayer;
	Claps clapsPlayer;

	NoteMapper noteMapper[Voice::count];
	uint8_t midiChannel = 10;									// Drum map channel 1-16 (0 = omni); 10 = BeatStep Pro drum track
	static constexpr uint8_t pitchRefNote = 60;					// Chromatic channels play the configured pitch at this note

	struct MixConfig {
		float duckDrums = 0.0f;				// Kick sidechain depth on snare, hats, toms, claps (0 - 1); off by default
		float duckSamplers = 0.0f;			// Depth on both samplers
		float duckReverb = 0.0f;			// Depth on reverb return
		float duckHoldMs = 25.0f;			// Time at full depth after each kick
		float duckReleaseMs = 160.0f;		// Release time constant
		float kickReverbSend = 0.0f;		// Kick into main reverb (0 = dry kick, rumble has its own path)

		// Master bus: glue compressor -> clipper (appended fields: older saved blocks keep these defaults)
		float compOn = 1.0f;				// 0 = compressor bypassed
		float compThreshold = -12.0f;		// dBFS (1.0 = DAC full scale)
		float compRatio = 3.0f;
		float compAttack = 15.0f;			// ms: slow enough to let the drum transients through
		float compRelease = 120.0f;			// ms
		float compKnee = 6.0f;				// dB
		float compMakeup = 3.0f;			// dB
		float compMix = 1.0f;				// Parallel (New York) blend: 0 = dry, 1 = fully compressed
		float compScHpf = 90.0f;			// Detector high-pass (Hz): sub energy pumps the bus less
		float compKick = 0.0f;				// 0 = kick bypasses the compressor (no pumping, still clipped), 1 = kick goes through it
		float clipMode = 1.0f;				// 0 = original tanh, 1 = clean (linear to -4.4 dB, soft knee), 2 = hard, 3 = warm (anti-aliased)
		float clipDrive = 2.0f;				// dB into the clipper
		float outTrim = 0.0f;				// dB after the clipper (up to +0.7 dB = DAC full scale)
		float inputTrim = 0.0f;				// dB on the whole mix before compressor and clipper (gain staging for hot pots)

		// Key & scale: tonal voices snap their pitch (config pitch, MIDI notes, tuning pots) to the key
		float keyRoot = -1.0f;				// -1 = off, 0 = C ... 11 = B
		float keyScale = 0.0f;				// see scaleMasks: 0 minor (Aeolian), 1 Dorian, 2 Phrygian, ...
		float tuneMode[Voice::count] = {2, 3, 0, 0, 0, 3, 0};	// per voice: 0 off, 1 root, 2 root + 5th, 3 scale, 4 pentatonic subset
	} mixConfig;

	struct Meter {
		float reductionDb;					// Maximum compressor gain reduction since last read
		float clipDb;						// Maximum level into the clipper (dB, > 0 = clipping)
		float peakDb;						// Maximum output level (dBFS)
		float cpuAvg;						// Audio interrupt load (%)
		float cpuMax;
		float underruns;
		float nanEvents;					// modulove: nan / inf recoveries since boot (all voices and the master bus)
	};
	void GetMeter(Meter& m);					// Reads and resets the peak values

	// Cycle profile per block (DWT counter): voices in Voice order, then reverb, master bus and control (triggers/buttons/sequencer)
	static constexpr uint32_t profCount = Voice::count + 3, profReverb = Voice::count, profMaster = Voice::count + 1, profControl = Voice::count + 2;
	float profAvg[profCount] = {};
	volatile float profMax[profCount] = {};
	void Profile(const uint32_t block, const uint32_t cycles) {
		profAvg[block] += 0.001f * (static_cast<float>(cycles) - profAvg[block]);
		if (cycles > profMax[block]) {
			profMax[block] = cycles;
		}
	}

	// Key & scale: returns hz snapped to the voice's allowed notes (unchanged when the key is off or the voice mode is 0)
	float Tune(const Voice v, const float hz) const;
	bool TuneActive(const Voice v) const { return tuneRoot >= 0 && tuneMask[v] != 0; }

	// Test and calibration helpers (console)
	void RequestEngine(const uint8_t voice, const uint8_t engine);	// Applied in the audio interrupt (blinks like a long press)
	void SetTestTone(const float freqHz, const float levelDb);		// freqHz 0 = off; level relative to the clipper ceiling
	const char* ButtonModeName();

	float duckLevel = 0.0f;										// Current duck amount (0 - 1)
	volatile uint32_t sampleClock = 0;							// Counts output samples (48kHz)
	float clockTickSamples = 0.0f;								// Smoothed MIDI clock tick length in samples (0 = no clock)

private:
	float FastTanh(const float x);
	void UpdateDuck();
	uint8_t config[Voice::count * 3 + 1];						// MIDI map: low/high note per voice, drum channel, pitch channel per voice

	float duckTarget = 0.0f;
	uint32_t duckHold = 0;
	float duckReleaseCoef = 0.9998f;
	static constexpr float duckAttackCoef = 0.02f;				// ~1ms attack avoids clicks on ducked voices
	uint32_t lastClockSample = 0;
	uint32_t lastKickSample = 0;
	float kickIntervalSamples = 0.0f;

	// Master bus dynamics
	dyn::Compressor compressor;
	dyn::Clipper clipper[2];
	float clipGain = 1.0f;
	float outGain = 1.0f;
	float inGain = 1.0f;
	uint16_t tuneMask[Voice::count] = {};						// allowed pitch classes relative to the key root (from tuneMode)
	int32_t tuneRoot = -1;
	float meterReduction = 0.0f, meterClip = 0.0f, meterPeak = 0.0f;

	// Long press engine switching and LED feedback. The hold is timed from the button level itself (not the
	// debounced edge), with 30 ms tolerance for contact bounce, in Play and Sequence switch positions.
	static constexpr uint32_t longPressMs = 700;
	static constexpr uint32_t blinkOnMs = 150, blinkPeriodMs = 350;
	uint32_t gpioIdr[11] = {};									// per-sample snapshot of the GPIO input registers (port A = 0 .. K = 10)
	uint8_t playTurn = 0;										// round robin start for pending hits (one hit set up per sample)
	bool holding[Voice::count] = {};
	bool holdFired[Voice::count] = {};
	uint32_t holdStart[Voice::count] = {};
	uint32_t lastHeld[Voice::count] = {};
	uint8_t shownEngine[Voice::count] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};	// 0xFF: blink once at boot
	bool blinkPreview[Voice::count] = {};
	uint8_t blinkCount[Voice::count] = {};
	uint32_t blinkStart[Voice::count] = {};
	uint32_t lastBlinkTick = 0;
	volatile uint8_t requestedEngine[Voice::count] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	void CheckLongPress(NoteMapper& note);
	void ShowBlinks();

	// Test tone for level calibration (console 'tone:freq,dB'): replaces the whole output
	volatile float testToneInc = 0.0f, testToneLevel = 0.0f;
	float testTonePhase = 0.0f;

	enum class ButtonMode {playNote, midiLearn, drumPattern};
	enum class MidiLearnState {off, lowNote, highNote};
	ButtonMode buttonMode;
	uint8_t midiLearnVoice;
	MidiLearnState midiLearnState;
	int16_t midiLearnCounter;
	uint32_t ledBackup[Voice::count];

};

extern VoiceManager voiceManager;
