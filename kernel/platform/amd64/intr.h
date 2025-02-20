#ifndef KRX_AMD64_INTR_H
#define KRX_AMD64_INTR_H

#include <stdint.h>

enum {
	kIntVecPageFault = 14,
	/* filtered at kIPLAST (1) */
	kIntVecAST = 31,
	/* filtered at kIPLDPC (2) */
	kIntVecDPC = 47,
	/* ... */
	kIntVecSyscall = 128,
	/*! filterable with IPL=clock (CR8=14) */
	kIntVecLAPICTimer = 224,
	/*! unfilterable */
	kIntVecIPIInvlPG = 240,
	kIntVecEnterDebugger = 255,
};

void lapic_eoi(void);
void lapic_enable(uint8_t spurvec);
uint32_t lapic_timer_calibrate(void);
void lapic_timer_start(void);
void lapic_pend(uint8_t vector);
void idt_load(void);

#endif /* KRX_AMD64_INTR_H */
