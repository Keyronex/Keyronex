/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sat Feb 15 2025.
 */
/*!
 * @file cpuid.h
 * @brief CPUID definitions.
 */

#ifndef KRX_AMD64_CPUID_H
#define KRX_AMD64_CPUID_H

#include <stdint.h>

struct cpuid_ret {
	uint32_t reg[4];
};

static inline struct cpuid_ret
cpuid(uint32_t leaf, uint32_t subleaf)
{
	struct cpuid_ret ret;
	asm volatile("cpuid"
		     : "=a"(ret.reg[0]), "=b"(ret.reg[1]), "=c"(ret.reg[2]),
		     "=d"(ret.reg[3])
		     : "a"(leaf), "c"(subleaf));
	return ret;
}

#endif /* KRX_AMD64_CPUID_H */
