#include "configManager.h"
#include "initialisation.h"
#include "USB.h"
#include "VoiceManager.h"
#include "FatTools.h"
#include "Reverb.h"


volatile uint32_t SysTickVal;		// 1 ms resolution
uint32_t i2sUnderrun = 0;				// Debug counter for I2S underruns
CpuLoad cpuLoad;
extern uint32_t SystemCoreClock;
volatile CrashRecord __attribute__((section (".noinit"))) crashRecord;	// survives the reset after a fault
uint32_t resetFlags = 0;

void InitCycleCounter()
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->LAR = 0xC5ACCE55;				// Cortex-M7: unlock DWT
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

// Create DMA buffer that need to live in non-cached memory area
volatile uint16_t __attribute__((section (".dma_buffer"))) ADC_array[ADC1_BUFFER_LENGTH + ADC2_BUFFER_LENGTH];

// Reverb delay lines are large so need to be placed in appropriate memory regions
Reverb __attribute__((section (".ram_d2_data"))) reverb;
float __attribute__((section (".ram_d1_data"))) reverbMixBuffer[94000];

// TODO:
// Sample panning (naming? web interface?)
// Performance updates
// Web editor: finish handling non-float values in config
// Sample playback does not account for samples stored in discontinuous memory locations


extern "C" {
#include "interrupts.h"
}


int main(void) {

	resetFlags = RCC->RSR;			// Reset reason (power on, brownout, pin, software / fault restart, watchdog)
	RCC->RSR |= RCC_RSR_RMVF;		// Clear the flags so the next reset shows only its own reason
	if (crashRecord.magic != crashMagic) {
		crashRecord.magic = crashMagic;			// first boot after power on: RAM content is random
		crashRecord.count = 0;
		crashRecord.type = 0;
	}

	InitClocks();					// Configure the clock and PLL
	InitHardware();
	extFlash.Init();				// Initialise external QSPI Flash
	configManager.RestoreConfig();	// Restore configuration settings (voice config, MIDI mapping, drum sequences)
	usb.Init(false);				// Pass false to indicate hard reset
	InitCycleCounter();				// CPU load meter
	InitI2S();						// Initialise I2S which will start main sample interrupts

	while (1) {
		usb.cdc.ProcessCommand();	// Check for incoming USB serial commands
		fatTools.CheckCache();		// Check if any outstanding cache changes need to be written to Flash
		voiceManager.IdleTasks();	// Check if filter coefficients need to be updated
		configManager.CheckAutoSave();	// Settings save themselves a few seconds after the last change

#if (USB_DEBUG)
		if (uart.commandReady) {
			uart.ProcessCommand();
		}
#endif

	}
}

