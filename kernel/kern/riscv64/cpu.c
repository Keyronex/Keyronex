/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#include "kern/ki.h"

int
ki_disable_interrupts(void)
{
	for (;;)
		;
}

void
ki_set_interrupts(int enabled)
{

	for (;;)
		;
}

void
ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	for (;;)
		;
}

void
ki_tlb_flush_vaddr_locally(vaddr_t addr)
{
	for (;;)
		;
}

void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
	for (;;)
		;
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
	for (;;)
		;
}

void
ke_set_tcb(uintptr_t tcb)
{
	kfatal("Unimplemented\n");
}

void
md_intr_frame_trace(md_intr_frame_t *frame)
{
	splraise(kIPLHigh);
	kprintf("Thread %p; CPU %p:\n", curthread(), curcpu());
	kfatal("Unimplemented\n");
}

void
md_switch(kthread_t *old_thread)
{
	for (;;)
		;
}

void
md_send_dpc_ipi(kcpu_t *cpu)
{
	for (;;)
		;
}
