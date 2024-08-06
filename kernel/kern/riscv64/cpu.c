/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#include "kdk/executive.h"
#include "kdk/riscv64.h"
#include "kern/ki.h"
#include "vm/vmp.h"

/* trap.S */
void asm_thread_trampoline(void);
void asm_swtch(riscv64_context_t **pold_sp, riscv64_context_t *psp);
/* vm_riscv64.c */
void write_satp(paddr_t paddr);

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
c_thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_release_nospl(&curcpu()->old_thread->lock);
	ki_set_interrupts(true);
	splx(kIPL0);
	func(arg);
}

void
ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	riscv64_context_t *ctx = (riscv64_context_t *)(thread->kstack_base +
	    KSTACK_SIZE - sizeof(riscv64_context_t));
	ctx->ra = (uintptr_t)asm_thread_trampoline;
	ctx->s0 = (uintptr_t)func;
	ctx->s1 = (uintptr_t)arg;
	thread->pcb.sp = ctx;
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
md_switch(kthread_t *old_thread, kthread_t *new_thread)
{
	bool intr;

	intr = ki_disable_interrupts();

	asm volatile("mv tp, %0" : : "r"(new_thread));
	write_satp(ex_curproc()->vm->md.table);

#if 0
	if (old_thread->user)
		save_fp(old_thread->pcb.fp);
	if (new_thread->user)
		restore_fp(new_thread->pcb.fp);
#endif

	asm_swtch(&old_thread->pcb.sp, new_thread->pcb.sp);
	ki_set_interrupts(intr);
}

void
md_send_dpc_ipi(kcpu_t *cpu)
{
	for (;;)
		;
}
