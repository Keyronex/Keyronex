/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief Kernel debug log for riscv. Uses SBI console.
 */

#include <asm/sbi.h>

#define COM0 0x3f8

void
ke_md_early_putc(int c, void *)
{
	sbi_ecall1(0x4442434E, 2, c);
}
