#include "kdk/executive.h"
#include "kdk/kern.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "kern/ki.h"
#include "vm/vmp.h"

void asm_swtch(void *old, void *new);
void asm_thread_trampoline();

void
save_fp(uint64_t *state)
{
	asm volatile("stp q0, q1, [%0, #0]\n"
		     "stp q2, q3, [%0, #32]\n"
		     "stp q4, q5, [%0, #64]\n"
		     "stp q6, q7, [%0, #96]\n"
		     "stp q8, q9, [%0, #128]\n"
		     "stp q10, q11, [%0, #160]\n"
		     "stp q12, q13, [%0, #192]\n"
		     "stp q14, q15, [%0, #224]\n"
		     "stp q16, q17, [%0, #256]\n"
		     "stp q18, q19, [%0, #288]\n"
		     "stp q20, q21, [%0, #320]\n"
		     "stp q22, q23, [%0, #352]\n"
		     "stp q24, q25, [%0, #384]\n"
		     "stp q26, q27, [%0, #416]\n"
		     "stp q28, q29, [%0, #448]\n"
		     "stp q30, q31, [%0, #480]\n"
		     "add %0, %0, #512\n"
		     "mrs x9, fpcr\n"
		     "mrs x10, fpsr\n"
		     "stp x9, x10, [%0, #0]\n"
		     : "=r"(state)
		     : "0"(state)
		     : "memory", "x9", "x10");
}

void
restore_fp(uint64_t *state)
{
	asm volatile("ldp q0, q1, [%0, #0]\n"
		     "ldp q2, q3, [%0, #32]\n"
		     "ldp q4, q5, [%0, #64]\n"
		     "ldp q6, q7, [%0, #96]\n"
		     "ldp q8, q9, [%0, #128]\n"
		     "ldp q10, q11, [%0, #160]\n"
		     "ldp q12, q13, [%0, #192]\n"
		     "ldp q14, q15, [%0, #224]\n"
		     "ldp q16, q17, [%0, #256]\n"
		     "ldp q18, q19, [%0, #288]\n"
		     "ldp q20, q21, [%0, #320]\n"
		     "ldp q22, q23, [%0, #352]\n"
		     "ldp q24, q25, [%0, #384]\n"
		     "ldp q26, q27, [%0, #416]\n"
		     "ldp q28, q29, [%0, #448]\n"
		     "ldp q30, q31, [%0, #480]\n"
		     "add %0, %0, #512\n"
		     "ldp x9, x10, [%0, #0]\n"
		     "msr fpcr, x9\n"
		     "msr fpsr, x10\n"
		     : "=r"(state)
		     : "0"(state)
		     : "memory", "x9", "x10");
}

void
md_switch(kthread_t *old_thread)
{
	kthread_t *new_thread = curthread();

	void write_ttbr0_el1(paddr_t val);
	write_ttbr0_el1(ex_curproc()->vm->md.table);

	if (old_thread->user)
		save_fp(old_thread->pcb.fp);
	if (new_thread->user)
		restore_fp(new_thread->pcb.fp);

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

vaddr_t invlpg_addr;
volatile uint32_t invlpg_done;

void
ki_tlb_flush_handler(void)
{
	ki_tlb_flush_vaddr_locally(invlpg_addr);
	__atomic_add_fetch(&invlpg_done, 1, __ATOMIC_RELEASE);
}

void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
	invlpg_addr = addr;
	__atomic_store_n(&invlpg_done, 1, __ATOMIC_RELEASE);

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	for (int i = 0; i < ncpus; i++) {
		if (cpus[i] == curcpu())
			continue;

		md_send_invlpg_ipi(cpus[i]);
	}

	while (__atomic_load_n(&invlpg_done, __ATOMIC_ACQUIRE) != ncpus)
		__asm__("isb");

	ki_tlb_flush_vaddr_locally(addr);
}

#if 0
void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
	/* todo smp */
	ki_tlb_flush_vaddr_locally(addr);
}
#endif

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
