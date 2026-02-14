/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file kcallout.c
 * @brief Kernel clock.
 */

#include <keyronex/cpu.h>

void kep_callout_hardclock();
void kep_disp_hardclock(void);

void
ke_hardclock(void)
{
	kep_disp_hardclock();
	kep_callout_hardclock();
}
