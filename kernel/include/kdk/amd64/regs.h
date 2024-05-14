#ifndef KRX_AMD64_REGS_H
#define KRX_AMD64_REGS_H

#include <stdint.h>

enum {
	kAMD64MSRAPICBase = 0x1b,
	kAMD64MSRTSCDeadline = 0x6e0,
	kAMD64MSRGSBase = 0xc0000101,
	kAMD64MSRKernelGSBase = 0xc0000102,
	kAMD64MSRFSBase = 0xc0000100
};

#define REG_FUNCS(type, regname)                                    \
	static inline type read_##regname()                         \
	{                                                           \
		type val;                                           \
		asm volatile("mov %%" #regname ", %0" : "=r"(val)); \
		return val;                                         \
	}                                                           \
	static inline void write_##regname(type val)                \
	{                                                           \
		asm volatile("mov %0, %%" #regname ::"a"(val));     \
	}

REG_FUNCS(uint64_t, cr0);
REG_FUNCS(uint64_t, cr2);
REG_FUNCS(uint64_t, cr3);
REG_FUNCS(uint64_t, cr4);
REG_FUNCS(uint64_t, cr8);

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

#endif /* KRX_AMD64_REGS_H */
