/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief Kernel debug log for amd64. Uses com0.
 */

#include <asm/io.h>

#define COM0 0x3f8

void
ke_md_early_putc(int c, void *)
{
	outb(COM0, c);
}
