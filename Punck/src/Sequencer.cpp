#include <Sequencer.h>

Sequencer sequencer;

Sequencer::Sequencer()
{
	// Create simple rock pattern for testing
	bar.beat[0][VoiceManager::kick].index = 0;
	bar.beat[0][VoiceManager::kick].level = 127;
	bar.beat[0][VoiceManager::hatClosed].index = 0;
	bar.beat[0][VoiceManager::hatClosed].level = 100;

	bar.beat[1][VoiceManager::hatClosed].index = 0;
	bar.beat[1][VoiceManager::hatClosed].level = 20;

	bar.beat[2][VoiceManager::snare].index = 0;
	bar.beat[2][VoiceManager::snare].level = 110;
	bar.beat[2][VoiceManager::hatClosed].index = 0;
	bar.beat[2][VoiceManager::hatClosed].level = 100;

	bar.beat[3][VoiceManager::hatClosed].index = 0;
	bar.beat[3][VoiceManager::hatClosed].level = 30;

	bar.beat[4][VoiceManager::kick].index = 0;
	bar.beat[4][VoiceManager::kick].level = 127;
	bar.beat[4][VoiceManager::hatClosed].index = 0;
	bar.beat[4][VoiceManager::hatClosed].level = 100;

	bar.beat[5][VoiceManager::hatClosed].index = 0;
	bar.beat[5][VoiceManager::hatClosed].level = 10;

	bar.beat[6][VoiceManager::snare].index = 0;
	bar.beat[6][VoiceManager::snare].level = 120;
	bar.beat[6][VoiceManager::hatClosed].index = 0;
	bar.beat[6][VoiceManager::hatClosed].level = 80;

	bar.beat[7][VoiceManager::hatClosed].index = 0;
	bar.beat[7][VoiceManager::hatClosed].level = 70;
}


void Sequencer::Start()
{
	if (!playing) {
		playing = true;
		position = 0;
		beatLen = 24000;
		currentBeat = 0;
	} else {
		playing = false;
	}
}


void Sequencer::Play()
{
	if (playing) {
		// Get tempo
		tempo = 0.9f * tempo + 0.1f * (0.5f * ADC_array[ADC_Tempo]);
		beatLen = 40000.0f - tempo;

		if (position == 0) {
			for (uint32_t i = 0; i < VoiceManager::voiceCount; ++i) {
				auto& b = bar.beat[currentBeat][i];
				if (b.level > 0) {
					auto& note = voiceManager.noteMapper[i];
					note.drumVoice->Play(note.voiceIndex, b.index, 0, static_cast<float>(b.level) / 127.0f);
				}
			}
		}

		if (++position > beatLen) {
			position = 0;
			if (++currentBeat >= beatsPerBar) {
				currentBeat = 0;
			}
		}

	}
}
