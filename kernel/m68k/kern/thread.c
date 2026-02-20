/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file thread.c
 * @brief m68k thread machdep.
 */

#include <stdint.h>

void
ke_md_enter_usermode(uintptr_t ip, uintptr_t sp)
{
	uint16_t sr;

	asm volatile("move.w %%sr, %0\n" : "=d"(sr));
	sr &= ~(1 << 13);

	asm volatile("move.l %0, %%usp\n\t"
		     "move.w #0, -(%%sp)\n\t" /* frame format 0, 4-word */
		     "move.l %1, -(%%sp)\n\t" /* pc */
		     "move.w %2, -(%%sp)\n\t" /* sr */
		     "rte\n"
		     :
		     : "a"(sp), "a"(ip), "a"(sr)
		     : "memory");
}
