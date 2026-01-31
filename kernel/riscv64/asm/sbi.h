/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file sbi.h
 * @brief Brief explanation.
 */


#ifndef ECX_ASM_SBI_H
#define ECX_ASM_SBI_H

#include <stdint.h>

typedef struct sbi_return {
	uintptr_t err, ret;
} sbi_return_t;

static inline sbi_return_t
sbi_ecall1(int ext, int func, uintptr_t arg0)
{
	register uintptr_t a7 asm("a7") = ext;
	register uintptr_t a6 asm("a6") = func;
	register uintptr_t a0 asm("a0") = arg0;
	register uintptr_t a1 asm("a1");
	asm volatile("ecall"
		     : "+r"(a0), "=r"(a1)
		     : "r"(a7), "r"(a6)
		     : "memory");
	return (sbi_return_t) { a0, a1 };
}

static inline sbi_return_t
sbi_ecall2(int ext, int func, uintptr_t arg0, uintptr_t arg1)
{
	register uintptr_t a7 asm("a7") = ext;
	register uintptr_t a6 asm("a6") = func;
	register uintptr_t a0 asm("a0") = arg0;
	register uintptr_t a1 asm("a1") = arg1;
	asm volatile("ecall"
		     : "+r"(a0), "+r"(a1)
		     : "r"(a7), "r"(a6)
		     : "memory");
	return (sbi_return_t) { a0, a1 };
}


#endif /* ECX_ASM_SBI_H */
