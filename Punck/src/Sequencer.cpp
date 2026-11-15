#include <Sequencer.h>

Sequencer sequencer;

Sequencer::Sequencer()
{
	// All five sequences start empty (upstream shipped rock/swing test patterns); fill them in the web editor
	for (auto& seq : sequence) {
		seq.info.bars = 1;
		seq.info.beatsPerBar = 16;
	}
}


bool Sequencer::IsEmpty(uint8_t seq)
{
	if (seq >= 5) {
		return true;
	}
	for (uint8_t bar = 0; bar < sequence[seq].info.bars && bar < 4; ++bar) {
		for (auto& beat : sequence[seq].bar[bar].beat) {
			for (auto& voice : beat) {
				if (voice.level > 0) {
					return false;
				}
			}
		}
	}
	return true;
}



void Sequencer::StartStop(uint8_t seq)
{
	if (!playing) {
		playing = true;
		activeSequence = seq;
		position = 0;
		currentBar = 0;
		currentBeat = 0;
		voiceManager.noteMapper[activeSequence].pwmLed.setMinLevel(currSeqBrightness);		// Specify minimum brightness level of led to show currently playing sequence
	} else {
		voiceManager.noteMapper[activeSequence].pwmLed.setMinLevel(0);
		if (seq == activeSequence) {
			playing = false;
		} else {
			voiceManager.noteMapper[seq].pwmLed.setMinLevel(currSeqBrightness);
			ChangeSequence(seq);
		}
	}
}


void Sequencer::Play()
{
	if (playing) {
		Sequence& seq = sequence[activeSequence];

		if (currentBar >= seq.info.bars) {
			currentBar = 0;
		}

		// Get tempo
		tempo = 0.9f * tempo + 0.1f * (0.25f * ADC_array[ADC_Tempo]);
		uint32_t beatLen = (18000.0f - tempo) * (16.0f / (float)seq.info.beatsPerBar);

		if (position == 0) {
			if (clockEveryTick || (currentBeat % (seq.info.beatsPerBar == 16 ? 4 : 6) == 0)) {
				GPIOD->ODR |= GPIO_ODR_OD9;					// PD9: activate tempo out
				clockOn = SysTickVal;
			}

			for (uint32_t i = 0; i < VoiceManager::Voice::count; ++i) {
				auto& b = seq.bar[currentBar].beat[currentBeat][i];
				if (b.level > 0) {
					auto& note = voiceManager.noteMapper[i];
					note.drumVoice->Play(note.voiceIndex, b.index, 0, static_cast<float>(b.level) / 127.0f, 1.0f);
				}
			}
		}

		// Turn off clock pulse after 6ms
		if (clockOn > 0 && SysTickVal > clockOn + 6) {
			clockOn = 0;
			GPIOD->ODR &= ~GPIO_ODR_OD9;				// PD9: clear tempo out pin
		}

		if (++position > beatLen) {
			position = 0;
			if (++currentBeat >= seq.info.beatsPerBar) {
				currentBeat = 0;
				++currentBar;
			}
		}

	}
}


void Sequencer::ChangeSequence(uint8_t seq)
{
	// Change playing sequence to synchronise time when changing between different beats per bar
	if (seq != activeSequence) {
		SeqInfo& oldSeq = sequence[activeSequence].info;
		SeqInfo& newSeq = sequence[seq].info;

		if (playing && oldSeq.beatsPerBar != newSeq.beatsPerBar) {

			// Normalise position
			float oldbeatLen = (18000.0f - tempo) * (16.0f / (float)oldSeq.beatsPerBar);
			float oldPos = (float)position + (oldbeatLen * currentBeat);
			float newbeatLen = (18000.0f - tempo) * (16.0f / (float)newSeq.beatsPerBar);

			currentBeat = std::floor(oldPos / newbeatLen);
			position = oldPos - (currentBeat * newbeatLen);
		}
		activeSequence = seq;
	}
}


Sequencer::SeqInfo Sequencer::GetSeqInfo(uint8_t seq)
{
	if (playing && seq != activeSequence) {
		StartStop(seq);						// Will call ChangeSequence and also handle switching LEDs
	} else {
		ChangeSequence(seq);				// Update active sequence so switching sequence in the editor updates playing sequence
	}
	return sequence[seq].info;
}



uint32_t Sequencer::GetBar(uint8_t** buff, uint8_t seq, uint8_t bar)
{
	*buff = (uint8_t*)&(sequence[seq].bar[bar]);
	return sizeof(sequence[seq].bar[bar]);
}


uint32_t Sequencer::GetSequences(uint8_t** buff)
{
	// return pointer to and size of complete set of sequences
	*buff = reinterpret_cast<uint8_t*>(&sequence);
	return sizeof(sequence);
}


void Sequencer::StoreSequences(uint8_t* buff)
{
	// Store complete set of sequences
	memcpy(sequence, buff, sizeof(sequence));
}


void Sequencer::StoreConfig(uint8_t* buff, uint32_t len, uint8_t seq, uint8_t bar, uint8_t beatsPerBar, uint8_t bars)
{
	sequence[seq].info.bars = bars;
	sequence[seq].info.beatsPerBar = beatsPerBar;
	if (len <= sizeof(sequence[seq].bar[bar])) {
		memcpy(&(sequence[seq].bar[bar]), buff, len);
	}
}


uint32_t Sequencer::SequencesSize()
{
	return sizeof(sequence);
}
