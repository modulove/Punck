#pragma once
// Host (PC) stand-in for Punck/src/initialisation.h: lets the real voice code compile and render to WAV
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cmath>

constexpr double pi = 3.14159265358979323846;
constexpr float intToFloatMult = 1.0f / 2147483648.0f;
constexpr float floatToIntMult = 2147483648.0f;
static constexpr uint32_t systemSampleRate = 48000;
static constexpr float systemMaxFreq = 22000.0f;
extern volatile uint32_t SysTickVal;

#define FLASH_CODE
#define ADC1_BUFFER_LENGTH 8
#define ADC2_BUFFER_LENGTH 7
extern volatile uint16_t ADC_array[ADC1_BUFFER_LENGTH + ADC2_BUFFER_LENGTH];

enum ADC_Controls {
	ADC_KickAttack = 0, ADC_KickDecay = 1, ADC_KickLevel = 2,
	ADC_SnareFilter = 3, ADC_SnareTuning = 4, ADC_SnareLevel = 5,
	ADC_SampleALevel = 6, ADC_SampleBLevel = 7, ADC_Tempo = 8,
	ADC_HiHatDecay = 9, ADC_HiHatLevel = 10,
	ADC_SampleAVoice = 11, ADC_SampleASpeed = 12, ADC_SampleBVoice = 13, ADC_SampleBSpeed = 14,
};
enum channel {left = 0, right = 1};

// Hardware random number generator: every read of RNG->DR returns a new value
uint32_t HostRandom();
struct HostRngReg { operator uint32_t() const { return HostRandom(); } };
struct HostRng { HostRngReg DR; };
extern HostRng* RNG;
