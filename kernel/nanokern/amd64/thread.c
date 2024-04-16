#include "kdk/amd64/regs.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "vm/vmp.h"

void
md_cpu_init(kcpu_t *cpu)
{
}

void
md_switch(kthread_t *old_thread)
{
	extern void asm_swtch(void * old, void * new);
	//write_cr3(curthread()->process->vm->md.table);
	asm_swtch(&old_thread->pcb, &curthread()->pcb);
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
	uint64_t *sp = thread->kstack_base + KSTACK_SIZE;
	memset(&thread->pcb, 0x0, sizeof(thread->pcb));
	//thread->pcb.rip = (uintptr_t)thread_trampoline;
	thread->pcb.rdi = (uint64_t)func;
	thread->pcb.rsi = (uint64_t)arg;
	sp-= 2;
	*sp = (uint64_t)thread_trampoline;
	thread->pcb.rsp = (uint64_t)(sp);
}
