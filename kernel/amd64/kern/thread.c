/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file thread.c
 * @brief AMD64 thread.
 */

#include <sys/cpulocal.h>
#include <sys/k_cpu.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/pcb.h>
#include <sys/x86.h>

void
kep_arch_set_tp(void *addr)
{
	wrmsr(AMD64_GSBASE_MSR, (uint64_t)addr);
}

void
ke_set_tcb(uintptr_t value)
{
	ke_curthread()->tcb = value;
	kdprintf("ke_set_tcb: setting TCB to %p\n", (void *)value);
	wrmsr(AMD64_FSBASE_MSR, value);
}

void
kep_thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_exit_nospl(&CPU_LOCAL_LOAD(prevthread)->lock);
	splx(IPL_0);
	func(arg);
}

void
ke_md_enter_usermode(uintptr_t ip, uintptr_t sp)
{
	uint64_t frame[5];

	frame[0] = ip;
	frame[1] = 0x38 | 0x3;
	frame[2] = 0x202;
	frame[3] = sp;
	frame[4] = 0x40 | 0x3;

	asm volatile("mov %0, %%rsp\n\t"
		     "swapgs\n\t"
		     "iretq\n"
	    :
	    : "r"(frame)
	    : "memory");
}
