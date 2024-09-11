#include "kdk/aarch64.h"
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
md_switch(kthread_t *old_thread, kthread_t *new_thread)
{
	bool intr;

	intr = ki_disable_interrupts();

	void write_ttbr0_el1(paddr_t val);
	write_ttbr0_el1(ex_curproc()->vm->md.table);

	if (old_thread->user)
		save_fp(old_thread->pcb.fp);
	if (new_thread->user)
		restore_fp(new_thread->pcb.fp);

	asm_swtch(&old_thread->pcb, &curthread()->pcb);
	ki_set_interrupts(intr);
}

void
thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_release_nospl(&curcpu()->old_thread->lock);
	ki_set_interrupts(true);
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

static inline size_t
icache_line()
{
	uint64_t ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	return (4 << (ctr & 0b1111));
}

static inline size_t
dcache_line()
{
	uint64_t ctr;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	return (4 << ((ctr >> 16) & 0b1111));
}

void
ki_dcache_clean_invalidate_range(vaddr_t base, vaddr_t limit)
{
	size_t dcline = dcache_line();

	for (vaddr_t it = ROUNDDOWN(base, dcache_line()); it < limit;
	     it += dcline)
		asm volatile("dc civac, %0" ::"r"(it) : "memory");

	asm volatile("dsb ish\n\t" ::: "memory");
}

/*!
 * @brief Invalidate dcache lines.
 */
void
ki_dcache_invalidate_range(vaddr_t base, vaddr_t limit)
{
	size_t dcline = dcache_line();

	for (vaddr_t it = ROUNDDOWN(base, dcache_line()); it < limit;
	     it += dcline)
		asm volatile("dc ivac, %0" ::"r"(it) : "memory");

	asm volatile("dsb ish\n\t" ::: "memory");
}

/* good for non-aliasing icache. */
void
ki_icache_synchronise_range(vaddr_t base, vaddr_t limit)
{
	size_t dcline = dcache_line(), icline = icache_line();

	for (vaddr_t it = ROUNDDOWN(base, dcache_line()); it < limit;
	     it += dcline)
		asm volatile("dc cvau, %0\n\t" ::"r"(it) : "memory");

	asm volatile("dsb ish\n\t" ::: "memory");

	for (vaddr_t it = ROUNDDOWN(base, icline); it < limit; it += icline)
		asm volatile("ic ivau, %0\n\t" ::"r"(it) : "memory");

	asm volatile("dsb ish\n\t"
		     "isb\n\t");
}

void
ki_tlb_flush_vaddr_locally(vaddr_t addr)
{
	if (addr == -1UL) {
		asm volatile("dsb st\n\t"
			     "tlbi vmalle1is\n\t"
			     "dsb sy\n\t"
			     "isb\n\t" ::
				 : "memory");
		return;
	}
	asm volatile("dsb st\n\t"
		     "tlbi vaae1, %0\n\t"
		     "dsb sy\n\t"
		     "isb\n\t"
		     :
		     : "r"(addr >> 12)
		     : "memory");
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

void ki_tlb_flush_globally(void)
{
	ki_tlb_flush_vaddr_globally(-1UL);
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
	ki_disable_interrupts();

	uintptr_t spsr = 0x60000000;
	uintptr_t sp_el1 = (uintptr_t)curthread()->kstack_base + KSTACK_SIZE;

	asm volatile("msr sp_el0, %0\n\t"
		     "msr elr_el1, %1\n\t"
		     "msr spsr_el1, %2\n\t"
		     "mov sp, %3\n\t"
		     "eret\n\t"
		     "dsb sy\n\t"
		     "isb\n\t"
		     :
		     : "r"(sp), "r"(ip), "r"(spsr), "r"(sp_el1)
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
	splraise(kIPLHigh);
	kprintf("Thread %p; CPU %p:\n", curthread(), curcpu());
	kfatal("Unimplemented\n");
}
