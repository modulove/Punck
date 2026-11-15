#pragma once

#include "initialisation.h"
#include "MidiHandler.h"
#include "Samples.h"
#include "Kick.h"
#include "Snare.h"
#include "HiHat.h"
#include "Toms.h"
#include "Claps.h"
#include <cstring>

struct NoteMapper {
	enum  TriggerType: uint8_t {noTrigger = 0, triggerBtn = 1, trigger1 = 2, trigger2 = 4};
	DrumVoice* drumVoice = nullptr;
	uint8_t voice;
	uint8_t voiceIndex = 0;				// For use with players that have multiple outputs (eg sampler)
	uint8_t midiLow;
	uint8_t midiHigh;
	uint8_t pitchChannel = 0;			// MIDI channel (1-16) on which the voice is played chromatically (0 = off)

	struct Trigger {
		GPIO_TypeDef* btnGpioBank;
		uint8_t btnGpioPin;
		GPIO_TypeDef* trgGpioBank;
		uint8_t trgGpioPin;
		GPIO_TypeDef* tr2GpioBank;
		uint8_t tr2GpioPin;

		uint8_t debounce = 0;
		bool buttonOn;

		bool ButtonHeld() {
			return btnGpioBank && (btnGpioBank->IDR & (1 << btnGpioPin)) == 0;
		}

		TriggerType IsOn()  {
			TriggerType triggers = (TriggerType)((btnGpioBank && (btnGpioBank->IDR & (1 << btnGpioPin)) == 0) ? 1 : 0 |
							   (trgGpioBank && (trgGpioBank->IDR & (1 << trgGpioPin)) == 0) ? 2 : 0 |
							   (tr2GpioBank && (tr2GpioBank->IDR & (1 << tr2GpioPin)) == 0) ? 4 : 0);
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

		uint32_t GetLevel()  {
			return *timerChannel;
		}
		void Level(float brightness)  {
			*timerChannel = std::max(minLevel, static_cast<uint32_t>(brightness * 4095.0f));
		}
		void setMinLevel(uint32_t level) {		// Sets LED current/minimum brightness: to display currently playing sequence with dimly illuminated button
			minLevel = level;
			*timerChannel = level;
		}
	} pwmLed;
};


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
	void CheckButtons();
	void IdleTasks();
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
	} mixConfig;

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

	// Long press engine switching and LED feedback
	static constexpr uint32_t longPressMs = 700;
	uint32_t pressStart[Voice::count] = {};
	bool pressActive[Voice::count] = {};
	uint8_t blinkCount[Voice::count] = {};
	uint32_t blinkStart[Voice::count] = {};
	void CheckLongPress(NoteMapper& note);
	void ShowBlinks();

	enum class ButtonMode {playNote, midiLearn, drumPattern};
	enum class MidiLearnState {off, lowNote, highNote};
	ButtonMode buttonMode;
	uint8_t midiLearnVoice;
	MidiLearnState midiLearnState;
	int16_t midiLearnCounter;
	uint32_t ledBackup[Voice::count];

};

extern VoiceManager voiceManager;
