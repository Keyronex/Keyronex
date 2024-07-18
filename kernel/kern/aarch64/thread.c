#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "kdk/vm.h"

extern void thread_trampoline();

#if 0
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
#endif

void
ki_tlb_flush_vaddr_locally(vaddr_t addr)
{
}

void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
}

void
ke_set_tcb(uintptr_t tcb)
{
}

void
md_intr_frame_trace(md_intr_frame_t *frame)
{
}

void md_send_dpc_ipi(kcpu_t *cpu)
{
}
