#include "kdk/executive.h"
#include "kdk/libkern.h"
#include "kdk/kern.h"

void
md_cpu_init(kcpu_t *cpu)
{
}

void vmp_activate(struct vm_procstate *ps);

void
md_switch(kthread_t *old_thread, kthread_t *new_thread)
{
	extern void asm_swtch(m68k_context_t * old, m68k_context_t * new);
	vmp_activate(ex_curproc()->vm);
	asm_swtch(&old_thread->pcb.genregs, &curthread()->pcb.genregs);
}

void
thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_release_nospl(&curcpu()->old_thread->lock);
	splx(kIPL0);
	func(arg);
}

void
ki_thread_copy_fpu_state(kthread_t *to)
{
	/* TODO */
}

void asm_thread_trampoline(void (*)(void *), void *);

void
ke_thread_init_context(kthread_t *thread, md_intr_frame_t *fork_frame,
    void (*func)(void *), void *arg)
{
	uint32_t *sp;

	memset(&thread->pcb.genregs, 0x0, sizeof(thread->pcb.genregs));
	thread->pcb.genregs.pc = (uintptr_t)asm_thread_trampoline;

	if (fork_frame == NULL) {
		sp = thread->kstack_base + KSTACK_SIZE - sizeof(uint32_t);
	} else {
		md_intr_frame_t *frame;

		sp = thread->kstack_base + KSTACK_SIZE - sizeof(*fork_frame);
		memcpy(sp, fork_frame, sizeof(*fork_frame));

		frame = (void*)sp;
		frame->d0 = 0;
	}

	*sp-- = (uint32_t)arg;
	*sp-- = (uint32_t)func;
	thread->pcb.genregs.sp = (uint32_t)sp;
	thread->pcb.genregs.sr = 0x2000;
	thread->tcb = 0;
}

void
ki_tlb_flush_vaddr_locally(uintptr_t vaddr)
{
	asm volatile("pflush (%0)" : : "a"(vaddr) : "memory");
}

void
ki_tlb_flush_vaddr_globally(uintptr_t vaddr)
{
	ki_tlb_flush_vaddr_locally(vaddr);
}

void
ki_tlb_flush_locally(void)
{
	asm volatile("pflusha" : : : "memory");
}

void
ki_tlb_flush_globally(void)
{
	ki_tlb_flush_locally();
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
	uint16_t sr;

#if 0
	kprintf("Entering usermode with usp 0x%zx\n", sp);
#endif

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

void
ki_trap_recover(md_intr_frame_t *frame)
{
	kfatal("Implement me!\n");
}

void
md_current_trace(void)
{
	for(;;) ;
}
