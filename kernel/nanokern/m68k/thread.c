#include "kdk/libkern.h"
#include "kdk/nanokern.h"

void
md_cpu_init(kcpu_t *cpu)
{
}

void vmp_activate(struct vm_procstate *ps);

void
md_switch(kthread_t *old_thread)
{
	extern void asm_swtch(m68k_context_t * old, m68k_context_t * new);
	vmp_activate(curthread()->process->vm);
	asm_swtch(&old_thread->pcb.genregs, &curthread()->pcb.genregs);
}

static void
thread_trampoline(void (*func)(void *), void *arg)
{
	splx(kIPL0);
	func(arg);
}

void
ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	uint32_t *sp = thread->kstack_base;
	memset(&thread->pcb.genregs, 0x0, sizeof(thread->pcb.genregs));
	thread->pcb.genregs.pc = (uintptr_t)thread_trampoline;
	sp = (uint32_t *)((uintptr_t)thread->kstack_base + KSTACK_SIZE -
	    sizeof(uint32_t));
	*sp-- = (uint32_t)arg;
	*sp-- = (uint32_t)func;
	*sp-- = 0x0;
	thread->pcb.genregs.sp = (uint32_t)sp;
	thread->pcb.genregs.sr = 0x2000;
}

void
ki_tlb_flush_vaddr_globally(uintptr_t vaddr)
{
	asm volatile("pflush (%0)" : : "a"(vaddr));
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
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
		     : "sp", "memory");
}
