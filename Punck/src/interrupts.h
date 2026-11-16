void OTG_FS_IRQHandler(void)
{
	usb.InterruptHandler();
}


// Detailed debug timings
uint32_t loopTime = 0, maxLoopTime = 0, outputTime = 0, maxOutputTime = 0, reverbTime = 0, maxReverbTime = 0;


void SPI2_IRQHandler()									// I2S Interrupt
{

#if (TIMINGDEBUG)
	loopTime = TIM3->CNT;
	if (loopTime > maxLoopTime && SysTickVal > 100) {
		maxLoopTime = loopTime;
	}
	TIM3->EGR |= TIM_EGR_UG;							// Re-initialize debug counter
#endif

	if ((SPI2->SR & SPI_SR_UDR) == SPI_SR_UDR) {		// Check for Underrun condition
		SPI2->IFCR |= SPI_IFCR_UDRC;					// Clear underrun condition
		++i2sUnderrun;
		return;
	}

	// modulove: write the frame prepared by the previous call first, then compute the next one. The FIFO gets its
	// data within a few cycles of asking, so a sample may take up to nearly a full period before the I2S runs dry
	// (upstream wrote after the triggers, reverb and master bus: 8-12 us into the interrupt). One sample latency.
	SPI2->TXDR = voiceManager.nextOut[0];
	SPI2->TXDR = voiceManager.nextOut[1];

	const uint32_t cycleStart = DWT->CYCCNT;
	voiceManager.Output();
	{
		// CPU load = cycles spent in Output / cycles between interrupts (self calibrating)
		const uint32_t used = DWT->CYCCNT - cycleStart;
		const uint32_t period = cycleStart - cpuLoad.lastStart;
		cpuLoad.lastStart = cycleStart;
		if (period > 1000 && period < 100000) {
			const float load = static_cast<float>(used) / static_cast<float>(period);
			cpuLoad.avg = cpuLoad.avg + 0.0005f * (load - cpuLoad.avg);
			if (load > cpuLoad.max) {
				cpuLoad.max = load;
			}
		}
	}

#if (TIMINGDEBUG)
	outputTime = TIM3->CNT;
	if (outputTime > maxOutputTime) {
		maxOutputTime = outputTime;
	}
#endif

}


void MDMA_IRQHandler()
{
	// fires when MDMA Flash to memory transfer has completed
	if (MDMA->GISR0 & MDMA_GISR0_GIF0) {
		MDMA_Channel0->CIFCR |= MDMA_CIFCR_CBTIF;		// Clear transfer complete interrupt flag
		usb.msc.DMATransferDone();
	}
}

uint32_t debugUartOR = 0;
void UART8_IRQHandler(void) {
	// MIDI Decoder
	if (UART8->ISR & USART_ISR_RXNE_RXFNE) {
		usb.midi.serialHandler(UART8->RDR); 			// accessing DR automatically resets the receive flag
	} else {
		// Overrun
		UART8->ICR |= USART_ICR_ORECF;					// The ORE bit is reset by setting the ORECF bit in the USART_ICR register
		++debugUartOR;
	}
}

// System interrupts
void NMI_Handler(void) {}

// modulove: faults record where they happened (crashRecord in .noinit, kept over the reset) and restart the module
// instead of hanging silently. frame = the stacked exception frame (r0-r3, r12, lr, pc, xpsr).
__attribute__((used, noinline)) void FaultRecord(const uint32_t* frame, const uint32_t type)
{
	if (crashRecord.magic != crashMagic) {
		crashRecord.magic = crashMagic;
		crashRecord.count = 0;
	}
	++crashRecord.count;
	crashRecord.type = type;
	crashRecord.pc = frame[6];
	crashRecord.lr = frame[5];
	crashRecord.cfsr = SCB->CFSR;
	crashRecord.hfsr = SCB->HFSR;
	crashRecord.mmfar = SCB->MMFAR;
	crashRecord.bfar = SCB->BFAR;
	crashRecord.uptimeMs = SysTickVal;
	__DSB();
	NVIC_SystemReset();
}

// Pass the active stack pointer (MSP or PSP, from EXC_RETURN bit 2) and the fault type to FaultRecord
#define FAULT_HANDLER(name, type) \
	__attribute__((naked)) void name(void) { \
		__asm volatile("tst lr, #4      \n" \
					   "ite eq          \n" \
					   "mrseq r0, msp   \n" \
					   "mrsne r0, psp   \n" \
					   "movs r1, #" #type " \n" \
					   "b FaultRecord   \n"); \
	}

FAULT_HANDLER(HardFault_Handler, 1)
FAULT_HANDLER(MemManage_Handler, 2)
FAULT_HANDLER(BusFault_Handler, 3)
FAULT_HANDLER(UsageFault_Handler, 4)

void SVC_Handler(void) {}

void DebugMon_Handler(void) {}

void PendSV_Handler(void) {}

void SysTick_Handler(void) {
	++SysTickVal;
}

/*
// USART Decoder
void USART3_IRQHandler() {
	//if ((USART3->ISR & USART_ISR_RXNE_RXFNE) != 0 && !uartCmdRdy) {
	if (!uartCmdRdy) {
		uartCmd[uartCmdPos] = USART3->RDR; 				// accessing RDR automatically resets the receive flag
		if (uartCmd[uartCmdPos] == 10) {
			uartCmdRdy = true;
			uartCmdPos = 0;
		} else {
			uartCmdPos++;
		}
	}
}*/
