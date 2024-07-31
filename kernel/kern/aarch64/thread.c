#include "kdk/executive.h"
#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

void asm_swtch(void *old, void *new);
void asm_thread_trampoline();

void
md_switch(kthread_t *old_thread)
{
	void write_ttbr0_el1(paddr_t val);
	write_ttbr0_el1(ex_curproc()->vm->md.table);
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
	asm volatile("dsb st");
	asm volatile("tlbi vale1, %0" : : "r"(addr >> 12) : "memory");
	asm volatile("dsb sy");
	asm volatile("isb");
}

void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
	/* todo smp */
	ki_tlb_flush_vaddr_locally(addr);
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
	uintptr_t spsr = 0x60000000;
	asm volatile("msr sp_el0, %0\n\t"
		     "msr elr_el1, %1\n\t"
		     "msr spsr_el1, %2\n\t"
		     "eret\n"
		     :
		     : "r"(sp), "r"(ip), "r"(spsr)
		     : "memory");
	kfatal("Unreached\n");
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
