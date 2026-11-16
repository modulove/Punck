#include <configManager.h>
#include "VoiceManager.h"
#include "Sequencer.h"
#include "reverb.h"




VoiceManager voiceManager;


VoiceManager::VoiceManager()
{
	for (uint8_t i = 0; i < Voice::count; ++i) {
		noteMapper[i].voice = i;
	}

	// Default MIDI map matches the BeatStep Pro drum track (channel 10, pads on notes 36 - 51, GM-like where possible).
	// Each voice also has a chromatic 'pitch channel' where note 60 plays the voice at its configured pitch.
	NoteMapper& k = noteMapper[Voice::kick];
	kickPlayer.noteMapper = &k;
	k.drumVoice = &kickPlayer;
	k.trigger = {GPIOB, 6, GPIOD, 1};
	k.pwmLed = {&TIM8->CCR4};			// PC9: Blue
	k.midiLow = 36;						// Pad 1
	k.midiHigh = 36;
	k.pitchChannel = 11;

	NoteMapper& s = noteMapper[Voice::snare];
	snarePlayer.noteMapper = &s;
	s.drumVoice = &snarePlayer;
	s.trigger = {GPIOB, 5, GPIOD, 3};
	s.pwmLed = {&TIM8->CCR3};			// PC8: Red
	s.midiLow = 37;						// Pads 2-3: tight, longer
	s.midiHigh = 38;
	s.pitchChannel = 12;

	NoteMapper& hh = noteMapper[Voice::hihat];
	hihatPlayer.noteMapper = &hh;
	hh.drumVoice = &hihatPlayer;
	hh.trigger = {GPIOE, 11, GPIOG, 10, GPIOD, 7};
	hh.pwmLed = {&TIM8->CCR2};			// PC7: White
	hh.midiLow = 42;					// Pads 7-9: closed, half open, open
	hh.midiHigh = 44;
	hh.pitchChannel = 13;

	NoteMapper& sa = noteMapper[samplerA];
	samples.sampler[0].noteMapper = &sa;
	sa.drumVoice = &samples;
	sa.voiceIndex = 0;
	sa.trigger = {GPIOB, 7, GPIOE, 14};
	sa.pwmLed = {&TIM4->CCR3};			// PD14: Green
	sa.midiLow = 45;					// Pads 10-12: bank A samples 1-3
	sa.midiHigh = 47;
	sa.pitchChannel = 16;				// Chromatic: plays the sample selected by the Voice pot

	NoteMapper& sb = noteMapper[samplerB];
	samples.sampler[1].noteMapper = &sb;
	sb.drumVoice = &samples;
	sb.voiceIndex = 1;
	sb.trigger = {GPIOG, 13, GPIOE, 15};
	sb.pwmLed = {&TIM4->CCR4};			// PD15: Yellow
	sb.midiLow = 48;					// Pads 13-16: bank B samples 1-4; range continues to 87 (whole bank from a DAW)
	sb.midiHigh = 87;

	NoteMapper& t = noteMapper[Voice::toms];
	tomsPlayer.noteMapper = &k;
	t.drumVoice = &tomsPlayer;
	t.midiLow = 40;						// Pads 5-6: low, high
	t.midiHigh = 41;
	t.pitchChannel = 14;

	NoteMapper& c = noteMapper[Voice::claps];
	clapsPlayer.noteMapper = &c;
	c.drumVoice = &clapsPlayer;
	c.midiLow = 39;						// Pad 4
	c.midiHigh = 39;
	c.pitchChannel = 15;

	StoreMixConfig(nullptr, 0);			// Calculate ducking coefficients from defaults
}




uint32_t leftOverflow = 0, rightOverflow = 0;	// Debug
//float leftOutput = 0.0f, rightOutput = 0.0f;
//float outInc = 0.005;
//uint32_t waitCrossing = 0;
constexpr float adjOffset = 0.0f;
constexpr float adjOutputScale = adjOutputScaleValue;
void VoiceManager::Output()
{
	const uint32_t controlCycles = DWT->CYCCNT;			// trigger inputs, buttons, sequencer: includes the per-hit setup they start
	CheckButtons();										// Handle buttons playing note or activating MIDI learn
	sequencer.Play();
	StartOneHit();										// one hit per sample from trigger inputs, buttons, MIDI, sequencer, console
	Profile(profControl, DWT->CYCCNT - controlCycles);

/*
	// Test code for calculating offsets and maximum levels before ADC distorts
	leftOutput += outInc;
	if (std::abs(leftOutput) > 1.0f) {
		leftOutput = (leftOutput > 0.0f) ? 1.0f : -1.0f;
		outInc *= -1.0f;
		waitCrossing = 0;
	}
	if (leftOutput <= 0.0f && outInc < 0.0f && waitCrossing < 10000) {
		leftOutput = 0.0f;
		waitCrossing++;
	}
	rightOutput = leftOutput;
*/

	++sampleClock;
	UpdateDuck();

	// Mix buses: kick stays dry and unducked; drums, samplers and reverb return are ducked by the kick (sidechain)
	// modulove: a voice output that is nan / inf is dropped and the voice re-initialised (RecoverVoice). Let into the bus
	// it poisoned compressor, clipper and reverb for good: silence with full scale DC on the output (stress test).
	float kickOut[2] = {kickPlayer.outputLevel[left], kickPlayer.outputLevel[right]};
	if (!Finite(kickOut[left]) || !Finite(kickOut[right])) {
		kickOut[left] = kickOut[right] = 0.0f;
		RecoverVoice(Voice::kick);
	}
	float drumsOut[2] = {0.0f, 0.0f};
	float samplesOut[2] = {samples.outputLevel[left], samples.outputLevel[right]};
	if (!Finite(samplesOut[left]) || !Finite(samplesOut[right])) {
		samplesOut[left] = samplesOut[right] = 0.0f;
		RecoverVoice(Voice::samplerA);
	}
	for (auto& nm : noteMapper) {
		if (nm.drumVoice != nullptr && nm.voiceIndex == 0 && nm.voice != Voice::kick && nm.voice != Voice::samplerA) {
			const float l = nm.drumVoice->outputLevel[left], r = nm.drumVoice->outputLevel[right];
			if (Finite(l) && Finite(r)) {
				drumsOut[left]  += l;
				drumsOut[right] += r;
			} else {
				RecoverVoice(nm.voice);
			}
		}
	}

	// reverb
#if (TIMINGDEBUG)
	uint32_t reverbStart = TIM3->CNT;
#endif

	const uint32_t reverbCycles = DWT->CYCCNT;
	float reverbL = 0.0f, reverbR = 0.0f;
	if (!reverbClearPending) {								// bypassed while the main loop empties the delay lines
		auto [rl, rr] = reverb.Process(drumsOut[left] + samplesOut[left] + mixConfig.kickReverbSend * kickOut[left],
									   drumsOut[right] + samplesOut[right] + mixConfig.kickReverbSend * kickOut[right]);
		if (Finite(rl) && Finite(rr)) {
			reverbL = rl;
			reverbR = rr;
		} else {
			++nanEvents[nanReverb];
			reverbClearPending = true;						// IdleTasks clears the reverb
		}
	}

#if (TIMINGDEBUG)
	reverbTime = TIM3->CNT - reverbStart;
	if (reverbTime > maxReverbTime && SysTickVal > 100) {
		maxReverbTime = reverbTime;
	}
#endif

	const float drumsGain   = 1.0f - mixConfig.duckDrums * duckLevel;
	const float samplesGain = 1.0f - mixConfig.duckSamplers * duckLevel;
	const float reverbGain  = 1.0f - mixConfig.duckReverb * duckLevel;

	const uint32_t masterCycles = DWT->CYCCNT;
	Profile(profReverb, masterCycles - reverbCycles);

	// Drum bus: everything except the dry kick (the kick joins the bus only if compKick is set)
	const float reverbOut[2] = {reverbL, reverbR};
	const bool kickThroughComp = mixConfig.compKick > 0.5f;
	float bus[2];
	for (uint8_t c = 0; c < 2; ++c) {
		kickOut[c] *= inGain;
		bus[c] = inGain * (drumsGain * drumsOut[c] + samplesGain * samplesOut[c] + reverbGain * reverbOut[c]) + (kickThroughComp ? kickOut[c] : 0.0f);
		if (!Finite(bus[c]) || !Finite(kickOut[c])) {		// overflow from extreme gains: drop the sample
			bus[c] = kickOut[c] = 0.0f;
			++nanEvents[nanBus];
		}
	}

	if (mixConfig.compOn > 0.5f) {
		const float gain = compressor.Gain(bus[left], bus[right]);
		if (Finite(gain)) {
			const float busGain = (1.0f - mixConfig.compMix) + mixConfig.compMix * gain;		// parallel blend
			bus[left] *= busGain;
			bus[right] *= busGain;
			meterReduction = std::max(meterReduction, compressor.ReductionDb());
		} else {
			compressor.Reset();
			++nanEvents[nanBus];
		}
	}

	float combinedOutput[2];
	for (uint8_t c = 0; c < 2; ++c) {
		combinedOutput[c] = bus[c] + (kickThroughComp ? 0.0f : kickOut[c]);
	}

	if (std::abs(combinedOutput[left])  > 1.0f) { ++leftOverflow; }		// Debug
	if (std::abs(combinedOutput[right]) > 1.0f) { ++rightOverflow; }

	if (mixConfig.clipMode < 0.5f) {
		combinedOutput[left] = FastTanh(combinedOutput[left]);		// Original soft clipping
		combinedOutput[right] = FastTanh(combinedOutput[right]);
	} else {
		const uint32_t mode = std::clamp<uint32_t>(static_cast<uint32_t>(mixConfig.clipMode + 0.5f), 1, 3);
		for (uint8_t c = 0; c < 2; ++c) {
			const float x = combinedOutput[c] * clipGain;
			meterClip = std::max(meterClip, std::abs(x));
			combinedOutput[c] = clipper[c].Process(x, mode) * outGain;
		}
	}
	if (testToneLevel > 0.0f) {
		testTonePhase += testToneInc;
		if (testTonePhase >= 1.0f) {
			testTonePhase -= 1.0f;
		}
		combinedOutput[left] = combinedOutput[right] = testToneLevel * std::sin(6.2831853f * testTonePhase);
	}
	for (uint8_t c = 0; c < 2; ++c) {
		if (!Finite(combinedOutput[c])) {					// last line of defence: never nan / full scale DC on the DAC
			combinedOutput[c] = 0.0f;
			clipper[c].Reset();
			++nanEvents[nanOut];
		}
	}
	meterPeak = std::max(meterPeak, std::max(std::abs(combinedOutput[left]), std::abs(combinedOutput[right])));


	Profile(profMaster, DWT->CYCCNT - masterCycles);
	const float outputScale = 2147483648.0f * adjOutputScale;
	// The frame goes out at the start of the next interrupt (SPI2_IRQHandler), straight after the I2S asked for it
	nextOut[left]  = (int32_t)((combinedOutput[left] + adjOffset) *  outputScale);
	nextOut[right] = (int32_t)((combinedOutput[right] + adjOffset) * outputScale);

	for (auto& nm : noteMapper) {
		if (nm.drumVoice != nullptr && nm.voiceIndex == 0) {			// If voiceIndex is > 0 drum voice has multiple channels (eg sampler)
#if (TIMINGDEBUG)
			uint32_t debugStart = TIM3->CNT;
#endif
			const uint32_t voiceCycles = DWT->CYCCNT;			// queued notes are started by StartOneHit (control profile)
			nm.drumVoice->CalcOutput();
			Profile(nm.voice, DWT->CYCCNT - voiceCycles);
#if (TIMINGDEBUG)
			nm.drumVoice->debugTime = TIM3->CNT - debugStart;
			if (nm.drumVoice->debugTime > nm.drumVoice->debugMaxTime) {
				nm.drumVoice->debugMaxTime = nm.drumVoice->debugTime;
			}
#endif
		}
	}
	ShowBlinks();									// After CalcOutput so the engine blink overrides the voice LEDs
}


// Algorithm source: https://varietyofsound.wordpress.com/2011/02/14/efficient-tanh-computation-using-lamberts-continued-fraction/
float VoiceManager::FastTanh(const float x)
{
	const float x2 = x * x;
	const float a = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2)));
	const float b = 135135.0f + x2 * (62370.0f + x2 * (3150.0f + x2 * 28.0f));
	return a / b;
}


void VoiceManager::KickTriggered(const float velocity)
{
	// Called from Kick::Play (always in the audio interrupt): softer kicks duck less
	const uint32_t now = sampleClock;
	const float interval = static_cast<float>(now - lastKickSample);
	lastKickSample = now;
	if (interval > 0.25f * systemSampleRate && interval < 2.0f * systemSampleRate) {
		kickIntervalSamples = interval;
	} else if (interval >= 2.0f * systemSampleRate) {
		kickIntervalSamples = 0.0f;
	}
	duckTarget = std::max(duckTarget, std::clamp(velocity, 0.0f, 1.0f));
	duckHold = static_cast<uint32_t>(mixConfig.duckHoldMs * 0.001f * systemSampleRate);
	tomsPlayer.KickPlayed(SixteenthSamples(), velocity);			// Bass engine: auto notes after each kick
}


void VoiceManager::UpdateDuck()
{
	if (duckHold > 0) {
		--duckHold;
	} else {
		duckTarget *= duckReleaseCoef;
	}
	duckLevel += (duckTarget - duckLevel) * duckAttackCoef;
}


void VoiceManager::MidiClock()
{
	// Measure MIDI clock tick length in samples (24 ticks per quarter note) - used for tempo-synced effects
	const uint32_t now = sampleClock;
	const float interval = static_cast<float>(now - lastClockSample);
	lastClockSample = now;
	if (interval > 200.0f && interval < 12000.0f) {				// 10 - 600 BPM
		clockTickSamples = (clockTickSamples == 0.0f) ? interval : 0.9f * clockTickSamples + 0.1f * interval;
	} else {
		clockTickSamples = 0.0f;								// Clock stopped or glitch: resynchronise
	}
}


void VoiceManager::NoteOn(MidiHandler::MidiNote midiNote, const uint8_t channel)
{
	if (midiNote.velocity == 0) {								// Note on with velocity 0 is a note off
		return;
	}
	const float velocity = static_cast<float>(midiNote.velocity) / 127.0f;

	// Chromatic pitch channels: note 60 plays the voice at its configured pitch
	bool pitched = false;
	for (auto& nm : noteMapper) {
		if (nm.pitchChannel != 0 && nm.pitchChannel == channel + 1 && nm.drumVoice != nullptr && buttonMode != ButtonMode::midiLearn) {
			pitched = true;
			const float ratio = std::exp2(static_cast<float>(static_cast<int32_t>(midiNote.noteValue) - pitchRefNote) / 12.0f);
			const uint32_t offset = (nm.voice == samplerA || nm.voice == samplerB) ? samples.KnobIndex(nm.voiceIndex) : 0;
			nm.drumVoice->QueuePlay(nm.voiceIndex, offset, 0, velocity, ratio);
		}
	}
	if (pitched || (midiChannel != 0 && midiChannel != channel + 1)) {
		return;
	}

	if (buttonMode == ButtonMode::midiLearn) {
		NoteMapper& n = noteMapper[midiLearnVoice];
		if (midiLearnState == MidiLearnState::lowNote) {
			n.midiLow = midiNote.noteValue;
			n.midiHigh = midiNote.noteValue;
			midiLearnState = MidiLearnState::highNote;
		} else {
			configManager.ScheduleSave();
			n.midiHigh = midiNote.noteValue;
			if (n.midiLow > n.midiHigh) {
				std::swap(n.midiLow, n.midiHigh);
			}

			// Switch to next voice Once high value is assigned
			midiLearnState = MidiLearnState::lowNote;
			for (auto& nm : noteMapper) {
				nm.pwmLed.Level(0.0f);
			}
			if (++midiLearnVoice == Voice::count) {
				midiLearnVoice = Voice::kick;
			}
		}
	} else {
		// Locate voice and queue note (notes are only triggered in the main interrupt to avoid data corruption)
		for (auto& note : noteMapper) {
			if (midiNote.noteValue >= note.midiLow && midiNote.noteValue <= note.midiHigh) {
				const uint32_t noteOffset = midiNote.noteValue - note.midiLow;
				const uint32_t noteRange = note.midiHigh - note.midiLow + 1;

				if (note.drumVoice) {
					note.drumVoice->QueuePlay(note.voiceIndex, noteOffset, noteRange, velocity, 1.0f);
				}
			}
		}
	}
}


void VoiceManager::CheckButtons()
{
	// Snapshot the input ports used by the switch, buttons and trigger inputs once per sample (B, C, D, E, G)
	gpioIdr[1] = GPIOB->IDR;
	gpioIdr[2] = GPIOC->IDR;
	gpioIdr[3] = GPIOD->IDR;
	gpioIdr[4] = GPIOE->IDR;
	gpioIdr[6] = GPIOG->IDR;

	// Check mode select switch. Options: Play note; MIDI learn; drum pattern selector
	ButtonMode oldMode = buttonMode;
	if ((gpioIdr[4] & GPIO_IDR_ID1) == 0) {				// PE1: MIDI Learn
		if (oldMode != ButtonMode::midiLearn) {
			buttonMode = ButtonMode::midiLearn;
			sequencer.playing = false;					// Stop active sequence
			midiLearnState = MidiLearnState::lowNote;
			midiLearnVoice = 0;
			midiLearnCounter = 0;
		} else {
			// Pulse LED to show MIDI Learn state - slow is low note, fast is high note
			midiLearnCounter += midiLearnState == MidiLearnState::lowNote ? 5 : 10;

			// Toms and claps do not have dedicated LEDs so pulse combinations
			if (midiLearnVoice == Voice::toms) {
				noteMapper[Voice::kick].pwmLed.Level(midiLearnCounter > 0 ? 1.0f : 0.0f);
				noteMapper[Voice::snare].pwmLed.Level(midiLearnCounter > 0 ? 1.0f : 0.0f);
			} else if (midiLearnVoice == Voice::claps) {
				noteMapper[Voice::samplerA].pwmLed.Level(midiLearnCounter > 0 ? 1.0f : 0.0f);
				noteMapper[Voice::samplerB].pwmLed.Level(midiLearnCounter > 0 ? 1.0f : 0.0f);
			} else {
				noteMapper[midiLearnVoice].pwmLed.Level(midiLearnCounter > 0 ? 1.0f : 0.0f);
			}
		}
	} else if ((gpioIdr[2] & GPIO_IDR_ID6) == 0) {		// PC6: Sequence Select
		buttonMode = ButtonMode::drumPattern;
	} else  {
		buttonMode = ButtonMode::playNote;
	}

	//	When switching to or from MIDI learn mode turn off all LEDs save if required and turn off LEDs
	if (oldMode != buttonMode && (oldMode == ButtonMode::midiLearn || buttonMode == ButtonMode::midiLearn)) {
		if (oldMode == ButtonMode::midiLearn && configManager.scheduleSave) {
			configManager.RequestSave();					// saved from the main loop (this runs in the audio interrupt)
		}
		for (auto& nm : noteMapper) {
			nm.pwmLed.setMinLevel(0);
		}
	}

	for (auto& note : noteMapper) {
		NoteMapper::TriggerType triggerType = note.trigger.IsOn(gpioIdr);
		if (triggerType) {
			if (!note.trigger.buttonOn) {
				note.trigger.buttonOn = true;

				if (buttonMode == ButtonMode::midiLearn) {
					noteMapper[midiLearnVoice].pwmLed.Level(0.0f);
					midiLearnState = MidiLearnState::lowNote;
					midiLearnVoice = note.voice;

				} else if (buttonMode == ButtonMode::playNote || triggerType != NoteMapper::TriggerType::triggerBtn) {
					// Trigger input only used to play notes; Trigger 2 currently only used for open hihat
					note.pendingPlay = (triggerType == NoteMapper::TriggerType::trigger2) ? 128 : 0;

				} else if (buttonMode == ButtonMode::drumPattern) {
					if (sequencer.IsEmpty(note.voice)) {
						note.pendingPlay = 0;							// No pattern stored on this button: play the sound instead
					} else {
						sequencer.StartStop(note.voice);
					}
				}
			}
		} else {
			note.trigger.buttonOn = false;
		}
		CheckLongPress(note);
	}

}


void VoiceManager::StartOneHit()
{
	// At most one hit is set up per sample, from any source: trigger inputs and buttons (pendingPlay), MIDI, sequencer
	// and console (queued notes). Several voices starting in the same sample would add up their per-hit setup and
	// overrun it. Round robin over the voices: the last of seven simultaneous hits starts 6 samples (125 us) late.
	for (uint32_t i = 0; i < Voice::count; ++i) {
		const uint32_t v = (playTurn + i) % Voice::count;
		NoteMapper& note = noteMapper[v];
		if (note.drumVoice == nullptr) {
			continue;
		}
		bool started = false;
		if (note.pendingPlay >= 0) {
			note.drumVoice->Play(note.voiceIndex, static_cast<uint32_t>(note.pendingPlay));
			note.pendingPlay = -1;
			started = true;
		} else {
			started = note.drumVoice->PlayQueuedSlot(note.voiceIndex);
		}
		if (started) {
			playTurn = static_cast<uint8_t>((v + 1) % Voice::count);
			return;
		}
	}
}


void VoiceManager::CheckLongPress(NoteMapper& note)
{
	// Holding a voice button for longPressMs switches to the next sound engine (ShowBlinks then blinks the number).
	// Timed from the raw button level so contact bounce at the press cannot cancel it; works in Play and Sequence.
	const uint8_t v = note.voice;
	DrumVoice* dv = note.drumVoice;
	if (dv == nullptr || note.voiceIndex != 0) {
		return;
	}

	// Engine change requested from the console: applied here, in the audio interrupt
	if (requestedEngine[v] != 0xFF) {
		if (requestedEngine[v] < dv->EngineCount()) {
			dv->SetEngine(requestedEngine[v]);
			blinkPreview[v] = true;
			configManager.ScheduleSave();
		}
		requestedEngine[v] = 0xFF;
	}

	if (dv->EngineCount() < 2) {
		return;
	}
	const uint32_t now = SysTickVal;
	if (note.trigger.ButtonHeld(gpioIdr)) {
		if (!holding[v]) {
			holding[v] = true;
			holdFired[v] = false;
			holdStart[v] = now;
		}
		lastHeld[v] = now;
		if (!holdFired[v] && buttonMode != ButtonMode::midiLearn && now - holdStart[v] >= longPressMs) {
			holdFired[v] = true;										// one switch per press: release to switch again
			dv->SetEngine((dv->GetEngine() + 1) % dv->EngineCount());
			blinkPreview[v] = true;
			configManager.ScheduleSave();							// remembered after power off
		}
	} else if (holding[v] && now - lastHeld[v] > 30) {
		holding[v] = false;
	}
}


void VoiceManager::ShowBlinks()
{
	// Whenever a voice's engine changes (long press, Tone Lab, preset, console, and once at boot) its LED blinks the
	// engine number: 150 ms on / 200 ms off. After a long press the new engine plays a preview hit.
	const uint32_t now = SysTickVal;
	const bool newTick = (now != lastBlinkTick);
	lastBlinkTick = now;

	for (auto& nm : noteMapper) {
		const uint8_t v = nm.voice;
		if (nm.pwmLed.timerChannel == nullptr || nm.drumVoice == nullptr || nm.voiceIndex != 0) {
			continue;
		}
		if (newTick && nm.drumVoice->EngineCount() > 1) {
			const uint8_t e = nm.drumVoice->GetEngine();
			if (e != shownEngine[v]) {
				shownEngine[v] = e;
				blinkCount[v] = e + 1;
				blinkStart[v] = now;
			}
		}
		if (blinkCount[v] == 0) {
			continue;
		}
		const uint32_t elapsed = now - blinkStart[v];
		if (elapsed >= blinkCount[v] * blinkPeriodMs) {
			blinkCount[v] = 0;
			nm.pwmLed.Level(0.0f);
			if (blinkPreview[v]) {
				blinkPreview[v] = false;
				nm.pendingPlay = 0;									// Preview the new engine (started by StartOneHit)
			}
		} else {
			nm.pwmLed.Level((elapsed % blinkPeriodMs) < blinkOnMs ? 1.0f : 0.0f);
		}
	}
}


void VoiceManager::RequestEngine(const uint8_t voice, const uint8_t engine)
{
	if (voice < Voice::count) {
		requestedEngine[voice] = engine;
	}
}


void VoiceManager::SetTestTone(const float freqHz, const float levelDb)
{
	if (freqHz <= 0.0f) {
		testToneLevel = 0.0f;
		return;
	}
	testToneInc = std::min(freqHz, 20000.0f) / systemSampleRate;
	testToneLevel = std::min(std::pow(10.0f, levelDb / 20.0f), 0.999f / adjOutputScaleValue);
}


const char* VoiceManager::ButtonModeName()
{
	switch (buttonMode) {
		case ButtonMode::playNote:		return "Play (middle)";
		case ButtonMode::drumPattern:	return "Sequence";
		case ButtonMode::midiLearn:		return "MIDI learn";
	}
	return "?";
}


float VoiceManager::SixteenthSamples()
{
	if (clockTickSamples > 0.0f && (sampleClock - lastClockSample) < systemSampleRate / 2) {
		return clockTickSamples * 6.0f;											// MIDI clock: 6 ticks per 16th
	}
	const float seq = sequencer.SixteenthSamples();
	if (seq > 0.0f) {
		return seq;
	}
	return kickIntervalSamples / 4.0f;											// Quarter note kicks -> 16ths (0 if unknown)
}


void VoiceManager::RecoverVoice(const uint32_t v)
{
	// A voice produced nan / inf: silence it and re-initialise its engine state (classic engines also reset their
	// filters on the next hit). Counted per voice for the console ('cpu') and the SysEx meter.
	++nanEvents[v];
	DrumVoice* dv = noteMapper[v].drumVoice;
	if (dv != nullptr) {
		dv->outputLevel[left] = dv->outputLevel[right] = 0.0f;
		dv->SetEngine(dv->GetEngine());
	}
}


void VoiceManager::IdleTasks()
{
	if (reverbClearPending) {								// nan / inf reached the reverb: empty it (the interrupt bypasses it)
		reverb.Clear();
		reverbClearPending = false;
	}
	for (auto& nm : noteMapper) {
		// If voiceIndex is > 0 drum voice has multiple channels (eg sampler)
		// Calculate filters even when not playing so ready for next hit (coefficients will only be updated if cut off has changed)
		if (nm.drumVoice != nullptr && nm.voiceIndex == 0) {
			nm.drumVoice->UpdateFilter();
		}
	}
}


uint32_t VoiceManager::GetConfig(uint8_t** buff)
{
	// Return a pointer to MIDI map config data for saving (upstream's buffer was one byte short so the channel was never saved)
	uint8_t i = 0;
	for (auto& nm : noteMapper) {
		config[i++] = nm.midiLow;
		config[i++] = nm.midiHigh;
	}
	config[i++] = midiChannel;
	for (auto& nm : noteMapper) {
		config[i++] = nm.pitchChannel;
	}
	*buff = config;
	return sizeof(config);
}


void VoiceManager::StoreConfig(const uint8_t* buff, const uint32_t len)
{
	// Reads MIDI map config data back into member values (tolerates shorter, older blocks)
	if (len < Voice::count * 2) {
		return;
	}
	uint32_t i = 0;
	for (auto& nm : noteMapper) {
		nm.midiLow = buff[i++];
		nm.midiHigh = buff[i++];
	}
	if (i < len) {
		midiChannel = std::min(buff[i++], (uint8_t)16);
	}
	for (auto& nm : noteMapper) {
		if (i < len) {
			nm.pitchChannel = std::min(buff[i++], (uint8_t)16);
		}
	}
}


uint32_t VoiceManager::GetMixConfig(uint8_t** buff)
{
	*buff = reinterpret_cast<uint8_t*>(&mixConfig);
	return sizeof(mixConfig);
}


void VoiceManager::StoreMixConfig(const uint8_t* buff, const uint32_t len)
{
	if (buff != nullptr) {
		memcpy(&mixConfig, buff, std::min(len, (uint32_t)sizeof(mixConfig)));
	}
	const float releaseSamples = std::max(mixConfig.duckReleaseMs, 1.0f) * 0.001f * systemSampleRate;
	duckReleaseCoef = std::exp(-1.0f / releaseSamples);

	auto& m = mixConfig;
	compressor.Configure(systemSampleRate, std::clamp(m.compThreshold, -60.0f, 0.0f), std::clamp(m.compRatio, 1.0f, 30.0f),
			std::clamp(m.compKnee, 0.0f, 24.0f), m.compAttack, m.compRelease, std::clamp(m.compMakeup, -12.0f, 24.0f), m.compScHpf);
	m.compMix = std::clamp(m.compMix, 0.0f, 1.0f);
	clipGain = std::pow(10.0f, std::clamp(m.clipDrive, -12.0f, 24.0f) / 20.0f);
	outGain = std::pow(10.0f, std::clamp(m.outTrim, -24.0f, maxOutTrimDb) / 20.0f);	// > 0 dB only up to DAC full scale
	inGain = std::pow(10.0f, std::clamp(m.inputTrim, -24.0f, 12.0f) / 20.0f);

	// Key & scale: allowed pitch classes per voice
	tuneRoot = (m.keyRoot < -0.5f) ? -1 : static_cast<int32_t>(m.keyRoot + 0.5f) % 12;
	const uint32_t scale = static_cast<uint32_t>(std::max(m.keyScale + 0.5f, 0.0f));
	for (uint32_t v = 0; v < Voice::count; ++v) {
		tuneMask[v] = harmony::ModeMask(static_cast<uint32_t>(std::clamp(m.tuneMode[v], 0.0f, 4.0f) + 0.5f), scale);
	}
}





float VoiceManager::Tune(const Voice v, const float hz) const
{
	if (tuneRoot < 0 || hz <= 0.0f || tuneMask[v] == 0) {
		return hz;
	}
	const float note = 69.0f + 12.0f * std::log2(hz / 440.0f);					// fractional MIDI note (precise: decides ties)
	return 440.0f * dyn::FastExp2((harmony::QuantizeNote(note, tuneRoot, tuneMask[v]) - 69.0f) / 12.0f);
}


void VoiceManager::GetMeter(Meter& m)
{
	auto toDb = [](const float x) { return 20.0f * std::log10(std::max(x, 1e-5f)); };
	m.reductionDb = meterReduction;
	m.clipDb = toDb(meterClip);
	m.peakDb = toDb(meterPeak);
	m.cpuAvg = cpuLoad.avg * 100.0f;
	m.cpuMax = cpuLoad.max * 100.0f;
	m.underruns = static_cast<float>(i2sUnderrun);
	uint32_t nans = 0;
	for (auto n : nanEvents) {
		nans += n;
	}
	m.nanEvents = static_cast<float>(nans);
	meterReduction = meterClip = meterPeak = 0.0f;
	cpuLoad.max = 0.0f;
}


void VoiceManager::SetAllLeds(float val)
{
	// Used so one-off processes like config storing can flash all LEDs simultaneously and then restore settings
	for (auto& nm : voiceManager.noteMapper) {
		if (nm.pwmLed.timerChannel) {
			ledBackup[nm.voice] = *nm.pwmLed.timerChannel;
			nm.pwmLed.Level(val);
		}
	}
}


void VoiceManager::RestoreAllLeds()
{
	// Restore leds to previous values after setting all
	for (auto& nm : voiceManager.noteMapper) {
		if (nm.pwmLed.timerChannel) {
			*nm.pwmLed.timerChannel = ledBackup[nm.voice];
		}
	}
}
