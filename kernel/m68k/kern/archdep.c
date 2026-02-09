/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file archdep.c
 * @brief Arch dependencies for m68k.
 */

#include <keyronex/cpu.h>

void
ke_arch_pause(void)
{
	asm volatile("nop");
}

kabstime_t
ke_time(void)
{
	return 0;
}

void
kep_arch_set_vbase(void)
{
	/* epsilon */
}

void
kep_arch_set_tp(void *)
{
	/* epsilon */
}

void
kep_arch_ipi_broadcast(void)
{
	/* epsilon */
}

void
kep_arch_ipi_unicast(kcpunum_t)
{
	/* epsilon */
}
