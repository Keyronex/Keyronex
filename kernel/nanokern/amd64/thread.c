#include "kdk/amd64/gdt.h"
#include "kdk/amd64/regs.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "vm/vmp.h"
#include "kdk/executive.h"

void
md_cpu_init(kcpu_t *cpu)
{
}

void
md_switch(kthread_t *old_thread)
{
	extern void asm_swtch(void * old, void * new);
	write_cr3(ex_curproc()->vm->md.table);
	wrmsr(kAMD64MSRFSBase, curthread()->tcb);
	curcpu()->cpucb.tss->rsp0 = ((uintptr_t)(curthread()->kstack_base)) +
	    KSTACK_SIZE;
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

void
ki_tlb_flush_vaddr_globally(vaddr_t addr)
{
	asm volatile("invlpg %0" : : "m"(*((const char *)addr)) : "memory");
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
