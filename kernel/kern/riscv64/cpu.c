/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#include "kern/ki.h"

static inline uint64_t
read_sstatus(void)
{
	uint64_t value;
	asm volatile("csrr %0, sstatus" : "=r"(value));
	return value;
}

static inline void
write_sstatus(uint64_t value)
{
	asm volatile("csrw sstatus, %0" : : "r"(value) : "memory");
}

int
ki_disable_interrupts(void)
{
	uint64_t sstatus = read_sstatus();
	uint64_t old_sstatus = sstatus;
	sstatus &= ~0x2; /* sie */
	write_sstatus(sstatus);
	return (old_sstatus & 0x2) != 0;
}

void
ki_set_interrupts(int enabled)
{
	uint64_t sstatus = read_sstatus();
	if (enabled)
		sstatus |= 0x2;
	else
		sstatus &= ~0x2;
	write_sstatus(sstatus);
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
	asm volatile("sfence.vma %0" : : "r"(addr) : "memory");
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
