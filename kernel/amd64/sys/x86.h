/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file cpulocal.h
 * @brief x86 register access functions.
 */

#ifndef ECX_KEYRONEX_X86_H
#define ECX_KEYRONEX_X86_H

#include <stdint.h>

enum {
	IA32_APIC_BASE_MSR = 0x1b,
	AMD64_GSBASE_MSR = 0xc0000101,
	AMD64_KERNELGSBASE_MSR = 0xc0000102,
	AMD64_FSBASE_MSR = 0xc0000100
};

#define REG_FUNCS(type, regname)				\
static inline type						\
read_##regname()						\
{								\
	type val;						\
	asm volatile("mov %%" #regname ", %0" : "=r"(val));	\
	return val;						\
}								\
static inline void						\
write_##regname(type val)					\
{								\
	asm volatile("mov %0, %%" #regname ::"a"(val));		\
}

REG_FUNCS(uint64_t, cr0);
REG_FUNCS(uint64_t, cr2);
REG_FUNCS(uint64_t, cr3);
REG_FUNCS(uint64_t, cr4);
REG_FUNCS(uint64_t, cr8);

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t high, low;
	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr) : "memory");
	return ((uint64_t)high << 32) | low;
}

static inline void
wrmsr(uint32_t msr, uint64_t value)
{
	uint32_t high = value >> 32;
	uint32_t low = value;

	asm volatile("wrmsr" ::"c"(msr), "d"(high), "a"(low));
}


#endif /* ECX_KEYRONEX_X86_H */
