#include "kdk/libkern.h"
#include "kdk/nanokern.h"

extern void thread_trampoline();

void
md_switch(kthread_t *old_thread)
{
	extern void asm_swtch(void *old, void *new);
	asm_swtch(&old_thread->pcb, &curthread()->pcb);
}

void
ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	uint64_t *sp = thread->kstack_base + KSTACK_SIZE;
	memset(&thread->pcb, 0x0, sizeof(thread->pcb));
	thread->pcb.genregs.x19 = (uint64_t)func;
	thread->pcb.genregs.x20 = (uint64_t)arg;
	thread->pcb.genregs.x30 = (uint64_t)thread_trampoline;
	thread->pcb.genregs.sp = (uint64_t)(sp);
}
