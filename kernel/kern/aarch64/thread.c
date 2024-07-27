#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "kdk/vm.h"

void asm_swtch(void *old, void *new);
void asm_thread_trampoline();

void
md_switch(kthread_t *old_thread)
{
	asm_swtch(&old_thread->pcb, &curthread()->pcb);
}

void
thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_release_nospl(&curcpu()->old_thread->lock);
	splx(kIPL0);
	func(arg);
}

void
ke_thread_init_context(kthread_t *thread, void (*func)(void *), void *arg)
{
	uint64_t *sp = thread->kstack_base + KSTACK_SIZE;
	memset(&thread->pcb, 0x0, sizeof(thread->pcb));
	thread->pcb.genregs.x19 = (uint64_t)func;
	thread->pcb.genregs.x20 = (uint64_t)arg;
	thread->pcb.genregs.x30 = (uint64_t)asm_thread_trampoline;
	thread->pcb.genregs.sp = (uint64_t)(sp);
}

void
ki_tlb_flush_vaddr_locally(vaddr_t addr)
{
	asm volatile("dsb ishst");
	asm volatile("tlbi vaae1, %0" : : "r"(addr >> 12) : "memory");
	asm volatile("dsb ish");
	asm volatile("isb");
}

void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
    kfatal("Unimplemented\n");
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
	kfatal("Unimplemented\n");
}

void
ke_set_tcb(uintptr_t tcb)
{
	kfatal("Unimplemented\n");
}

void
md_intr_frame_trace(md_intr_frame_t *frame)
{
	kfatal("Unimplemented\n");
}

void md_send_dpc_ipi(kcpu_t *cpu)
{
	kfatal("Unimplemented\n");
}
