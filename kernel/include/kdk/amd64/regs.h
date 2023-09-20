#ifndef KRX_AMD64_REGS_H
#define KRX_AMD64_REGS_H

#include <stdint.h>

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

#endif /* KRX_AMD64_REGS_H */
