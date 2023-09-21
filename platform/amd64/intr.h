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
};

enum {
	kAMD64MSRAPICBase = 0x1b,
	kAMD64MSRTSCDeadline = 0x6e0,
	kAMD64MSRGSBase = 0xc0000101,
	kAMD64MSRKernelGSBase = 0xc0000102,
	kAMD64MSRFSBase = 0xc0000100
};

static inline void
wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t high = value >> 32;
	uint32_t low = value;

	asm volatile("wrmsr" ::"c"(msr), "d"(high), "a"(low));
}

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t high, low;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr) : "memory");
	return ((uint64_t)high << 32) | low;
}

void lapic_eoi(void);
void lapic_enable(uint8_t spurvec);
uint32_t lapic_timer_calibrate(void);
void lapic_timer_start(void);
void idt_load(void);

#endif /* KRX_AMD64_INTR_H */
