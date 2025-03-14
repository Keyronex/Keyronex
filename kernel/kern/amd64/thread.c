#include "kdk/amd64.h"
#include "kdk/amd64/gdt.h"
#include "kdk/amd64/regs.h"
#include "kdk/executive.h"
#include "kdk/kern.h"
#include "kdk/libkern.h"
#include "kern/ki.h"
#include "vm/vmp.h"

extern void asm_swtch(void *old, void *new);

void
md_cpu_init(kcpu_t *cpu)
{
}

static inline void
fxsave(uint8_t *state)
{
	asm volatile("fxsave %0" : "+m"(*state) : : "memory");
}

static inline void
fxrstor(uint8_t *state)
{
	asm volatile("fxrstor %0" : : "m"(*state) : "memory");
}

void
ki_thread_copy_fpu_state(kthread_t *to)
{
	ipl_t ipl = spldpc();
	fxsave(to->pcb.fpu);
	splx(ipl);
}

void
md_switch(kthread_t *old_thread, kthread_t *new_thread)
{
	write_cr3(ex_curproc()->vm->md.table);

	wrmsr(kAMD64MSRFSBase, new_thread->tcb);

	curcpu()->cpucb.tss->rsp0 = ((uintptr_t)(new_thread->kstack_base)) +
	    KSTACK_SIZE;

	if (old_thread->user)
		fxsave(old_thread->pcb.fpu);
	if (new_thread->user)
		fxrstor(new_thread->pcb.fpu);

	asm_swtch(&old_thread->pcb, &new_thread->pcb);
}

void
thread_trampoline(void (*func)(void *), void *arg)
{
	ke_spinlock_release_nospl(&curcpu()->old_thread->lock);
	splx(kIPL0);
	func(arg);
}

void asm_thread_trampoline(void);

void
ke_thread_init_context(kthread_t *thread, md_intr_frame_t *fork_frame,
    void (*func)(void *), void *arg)
{
	uint64_t *sp;

	memset(&thread->pcb, 0x0, sizeof(thread->pcb));

	if (fork_frame == NULL) {
		sp = thread->kstack_base + KSTACK_SIZE;
	} else {
		md_intr_frame_t *frame;

		sp = thread->kstack_base + KSTACK_SIZE - sizeof(*fork_frame);
		memcpy(sp, fork_frame, sizeof(*fork_frame));

		frame = (md_intr_frame_t *)sp;
		frame->rax = 0;
	}

	thread->pcb.rdi = (uint64_t)func;
	thread->pcb.rsi = (uint64_t)arg;

	sp-= 2;
	*sp = (uint64_t)asm_thread_trampoline;
	thread->pcb.rsp = (uint64_t)(sp);
	thread->tcb = 0;
}

vaddr_t invlpg_addr;
volatile uint32_t invlpg_done;

void
ki_tlb_flush_vaddr_locally(vaddr_t addr)
{
	if (addr == -1UL) {
		write_cr3(read_cr3());
	} else
		asm volatile("invlpg %0"
			     :
			     : "m"(*((const char *)addr))
			     : "memory");
}

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
		__asm__("pause");

	ki_tlb_flush_vaddr_locally(addr);
}

void
ki_tlb_flush_globally(void)
{
	ki_tlb_flush_vaddr_globally(-1UL);
}

void
ki_enter_user_mode(uintptr_t ip, uintptr_t sp)
{
	uint64_t frame[5];

	frame[0] = ip;
	frame[1] = 0x38 | 0x3;
	frame[2] = 0x202;
	frame[3] = sp;
	frame[4] = 0x40 | 0x3;

	asm volatile("mov %0, %%rsp\n\t"
		     "swapgs\n\t"
		     "iretq\n"
		     :
		     : "r"(frame)
		     : "memory");
}

void
ke_set_tcb(uintptr_t tcb)
{
	wrmsr(kAMD64MSRFSBase, tcb);
}

void
ki_trap_recover(md_intr_frame_t *frame)
{
	ktrap_recovery_frame_t *recovery = &(curthread()->trap_recovery);

	frame->rax = 1;
	frame->rbx = recovery->rbx;
	frame->rbp = recovery->rbp;
	frame->r12 = recovery->r12;
	frame->r13 = recovery->r13;
	frame->r14 = recovery->r14;
	frame->r15 = recovery->r15;
	frame->rsp = recovery->rsp + 8; /* effect a return */
	frame->rip = recovery->rip;
}

void
md_current_trace(void)
{
	struct frame {
		struct frame *rbp;
		uint64_t rip;
	} *frame = (struct frame *)__builtin_frame_address(0);
	const char *name = NULL;
	size_t offs = 0;

	kprintf("Begin stack trace:\n");

	if (frame != NULL) {
		// resolve_symbol(frame->rip, &name, &offs);
		kprintf(" - %p %s+%lu\n", (void *)frame->rip,
		    name ? name : "???", offs);
	}

	while (frame && (uint64_t)frame >= 0xffff80000000UL &&
	    frame->rip != 0) {
		frame = frame->rbp;
		if (frame == NULL)
			break;

		// resolve_symbol(frame->rip, &name, &offs);
		kprintf(" - %p %s+%lu\n", (void *)frame->rip,
		    name ? name : "???", offs);
	}
}
