/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file thread.c
 * @brief AMD64 thread.
 */

#include <keyronex/cpu.h>
#include <keyronex/cpulocal.h>
#include <keyronex/intr.h>
#include <keyronex/ktask.h>
#include <keyronex/pcb.h>
#include <keyronex/x86.h>

void
kep_arch_set_tp(void *addr)
{
	wrmsr(AMD64_GSBASE_MSR, (uint64_t)addr);
}

void
kep_thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_exit_nospl(&CPU_LOCAL_LOAD(prevthread)->lock);
	splx(IPL_0);
	func(arg);
}
