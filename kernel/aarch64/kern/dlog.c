/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief Kernel debug log for AArch64. Hardcoded (For qemu virt?)
 */


volatile char *uart = (volatile char *)0xffff000000000000 + 0x09000000;

void
ke_md_early_putc(int c, void *)
{
	*uart = c;
}
