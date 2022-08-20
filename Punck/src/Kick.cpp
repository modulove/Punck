#include <Kick.h>
#include <cmath>
#include "Filter.h"

Kick kickPlayer;

Kick::Kick()
{
	// Store note handler voice for managing LEDs etc
	noteHandlerVoice = NoteHandler::kick;
}

void Kick::Play(uint32_t noteOffset, uint32_t noteRange)
{
	phase = Phase::Ramp1;
	position = 0.0f;
	currentLevel = 0.0f;

	noteHandler.VoiceLED(noteHandlerVoice, true);
}


void Kick::CalcSamples()
{
	switch (phase) {
	case Phase::Ramp1: {
		float inc = 0.22f;
		currentLevel = currentLevel + (inc * (1.0f - currentLevel));

		if (currentLevel > 0.6) {
			phase = Phase::Ramp2;
			GPIOG->ODR |= GPIO_ODR_OD11;		// PG11: debug pin green
		}
	}
		break;

	case Phase::Ramp2: {

		float inc = 0.015f;
		currentLevel = currentLevel + (inc * (1.0f - currentLevel));

		if (currentLevel > 0.82) {
			phase = Phase::Ramp3;
			GPIOG->ODR &= ~GPIO_ODR_OD11;

		}
	}

		break;

	case Phase::Ramp3: {

		float inc = 0.3f;
		currentLevel = currentLevel + (inc * (1.0f - currentLevel));

		if (currentLevel > 0.93) {
			fastSinInc = 0.017f;
			position = 2.0f;
			phase = Phase::FastSine;

		}
	}
		break;

	case Phase::FastSine:
		position += fastSinInc;
		currentLevel = std::sin(position);

		if (position >= 1.5f * pi) {
			slowSinInc = 0.0065f;
			slowSinLevel = 1.0f;
			phase = Phase::SlowSine;
		}
		break;

	case Phase::SlowSine: {
		slowSinInc *= 0.999992;			// Sine wave slowly decreases in frequency
		position += slowSinInc;			// Set current poition in sine wave
		float decaySpeed = 0.9994 + 0.00055f * (float)ADC_array[ADC_KickDecay] / 65536.0f;
		slowSinLevel = slowSinLevel * decaySpeed;
		currentLevel = std::sin(position) * slowSinLevel;

		if (slowSinLevel <= 0.00001f) {
			phase = Phase::Off;
			//currentLevel = 0.0f;
			noteHandler.VoiceLED(noteHandlerVoice, false);		// Turn off LED
		}
	}
		break;

	default:
		break;
	}

	ouputLevel = filter.CalcFilter(currentLevel, left);

}


