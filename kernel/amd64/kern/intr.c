/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief AMD64 interrupt handling.
 */

#include <sys/k_cpu.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/pcb.h>
#include <sys/vm_types.h>
#include <sys/x86.h>

#include <kern/defs.h>
#include <keyronex/syscall.h>
#include <libkern/lib.h>

#include "asm_intr.h"

enum posix_syscall;

struct __attribute__((packed)) idt_entry {
	uint16_t isr_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t type;
	uint16_t isr_mid;
	uint32_t isr_high;
	uint32_t zero;
};

#define EXTERN_ISR(VAL) void *isr_##VAL(void);
SPEC_IDT_ENTRIES(EXTERN_ISR)
IDT_ENTRIES(EXTERN_ISR)

void ke_hardclock(void);
void kep_dispatch_softints(ipl_t newipl);
uintptr_t sys_dispatch(karch_trapframe_t *frame, enum posix_syscall syscall,
    uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
    uintptr_t arg5, uintptr_t arg6, uintptr_t *out1);

void lapic_eoi(void);

static struct idt_entry bsp_idt[256];
static LIST_HEAD(, kirq) irqs[256];

bool
ke_arch_disable(void)
{
	uintptr_t flags;
	asm volatile("pushfq\n\t"
	    "popq %0\n\t"
	    "cli"
	    : "=r"(flags)
	    :
	    : "memory");
	return (flags & 0x200) != 0;
}

void
ke_arch_enable(bool enable)
{
	if (enable)
		asm volatile("sti" ::: "memory");
	else
		asm volatile("cli" ::: "memory");
}

ipl_t
ke_ipl(void)
{
	ipl_t ipl;
	asm volatile("movl %%gs:%c1, %0"
	    : "=r"(ipl)
	    : "i"(offsetof(struct kcpu_data, ipl))
	    : "memory");
	return ipl;
}

ipl_t
splraise(ipl_t newipl)
{
	bool intx = ke_arch_disable();
	ipl_t oldipl;

	if (newipl > IPL_DISP) {
		/* raise CR8 accordingly */
		asm volatile("mov %0, %%cr8"
		    :
		    : "r"((uint64_t)newipl)
		    : "memory");
	}

	oldipl = newipl;
	asm volatile("xchg %0, %%gs:%c1"
	    : "+r"(oldipl)
	    : "i"(offsetof(struct kcpu_data, ipl))
	    : "memory");

	ke_arch_enable(intx);
	return oldipl;
}

static inline uint32_t
pending_soft_ints(void)
{
	uint32_t v;
	asm volatile("movl %%gs:%c1, %0"
	    : "=r"(v)
	    : "i"(offsetof(struct kcpu_data, pending_soft_ints)));
	return v;
}

void
splx(ipl_t ipl)
{
	bool intx = ke_arch_disable();
	ipl_t previpl = ipl;

	asm volatile("xchg %0, %%gs:%c1"
	    : "+r"(previpl)
	    : "i"(offsetof(struct kcpu_data, ipl))
	    : "memory");

	kassert(previpl >= ipl, "splx: to higher IPL");

	if (previpl > IPL_DISP) {
		uint64_t cr8 = (ipl <= IPL_DISP) ? 0ULL : (uint64_t)ipl;
		asm volatile("mov %0, %%cr8" : : "r"(cr8) : "memory");
	}

	ke_arch_enable(intx);

	if ((pending_soft_ints() >> (uint64_t)ipl) != 0ULL) {
		kep_dispatch_softints(ipl);
	}
}

static void
idt_set(struct idt_entry *idt, uint8_t index, void *isr, uint8_t type,
    uint8_t ist)
{
	idt[index].isr_low = (uint64_t)isr & 0xFFFF;
	idt[index].isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
	idt[index].isr_high = (uint64_t)isr >> 32;
	idt[index].selector = 0x28; /* sixth */
	idt[index].type = type;
	idt[index].ist = ist;
	idt[index].zero = 0x0;
}

void
kep_arch_set_vbase(bool bsp)
{
	struct {
		uint16_t limit;
		uintptr_t addr;
	} __attribute__((packed)) idtr = {
		.limit = sizeof(bsp_idt) - 1,
		.addr = (uintptr_t)bsp_idt,
	};

	if (bsp) {
#define IDT_SET(VAL) idt_set(bsp_idt, VAL, &isr_##VAL, 0x8e, 0);
		SPEC_IDT_ENTRIES(IDT_SET);
		IDT_ENTRIES(IDT_SET);
		idt_set(bsp_idt, 0x80, &isr_128, 0xee, 0);

		for (size_t i = 0; i < 256; i++)
			LIST_INIT(&irqs[i]);
	}

	asm volatile("lidt %0" : : "m"(idtr));
}

void
kep_amd64_interrupt(karch_trapframe_t *frame, uintptr_t num)
{
	switch (num) {
	case 14: {
		uintptr_t cr2 = read_cr2();
		vm_prot_t prot = VM_READ;

		kassert(ke_ipl() < IPL_DISP);

		ke_arch_enable(1);

		if (frame->code & (1 << 1))
			prot |= VM_WRITE;
		if (frame->code & (1 << 4))
			prot |= VM_EXEC;
		if (frame->code & (1 << 2))
			prot |= VM_USER;

		void vm_fault(vaddr_t va, vm_prot_t prot);
		vm_fault(cr2, prot);

		ke_arch_disable();
		break;
	}

	case 128: {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
		ke_arch_enable(1);
		frame->rax = sys_dispatch(frame, frame->rax, frame->rdi,
		    frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9,
		    &frame->rdi);
#pragma GCC diagnostic pop
		break;
	}

	case 224: {
		ipl_t ipl = splhigh();
		ke_hardclock();
		lapic_eoi();
		splx(ipl);
		break;
	}

	case 225: {
		ipl_t ipl = splhigh();
		void kep_xcall_handler(void *unused);
		kep_xcall_handler(NULL);
		lapic_eoi();
		splx(ipl);
		break;
	}

	default:
		if (num >= 48 && num < 224) {
			ipl_t ipl = splhigh();
			struct kirq *entry;
			if (LIST_EMPTY(&irqs[num]))
				kfatal("unhandled irq %lu\n", num);
			LIST_FOREACH(entry, &irqs[num], list_entry)
				entry->handler(entry->arg);
			lapic_eoi();
			splx(ipl);
			break;
		}
		kfatal("int %lu\n", num);
	}
}

int
ke_amd64_idt_alloc(struct kirq_source *source, kirq_t *entry,
    kirq_handler_t *handler, void *arg, uint8_t *vec, kcpunum_t *cpu_out)
{
	static kspinlock_t irq_lock;
	ipl_t ipl;

	ipl = splhigh();
	ke_spinlock_enter_nospl(&irq_lock);

	for (int i = 48; i < 224; i++) {
		if (LIST_EMPTY(&irqs[i]) || (!source->edge)) {
			entry->source = *source;
			entry->handler = handler;
			entry->arg = arg;
			entry->cpu = CPU_LOCAL_LOAD(cpu_num);
			*cpu_out = entry->cpu;
			*vec = i;
			LIST_INSERT_HEAD(&irqs[i], entry, list_entry);
			ke_spinlock_exit(&irq_lock, ipl);
			return 0;
		}
	}

	ke_spinlock_exit(&irq_lock, ipl);
	return -1;
}

void kep_amd64_asm_switch(struct karch_pcb *old, struct karch_pcb *new);
void kep_amd64_asm_thread_trampoline(void);

void
fxsave(uint8_t *state)
{
	asm volatile("fxsave %0" : "+m"(*state) : : "memory");
}

void
fxrstor(uint8_t *state)
{
	asm volatile("fxrstor %0" : : "m"(*state) : "memory");
}

void
kep_arch_switch(struct kthread *old, struct kthread *next)
{
	wrmsr(AMD64_FSBASE_MSR, next->tcb);
	CPU_LOCAL_LOAD(arch.tss)->rsp0 = (uintptr_t)next->kstack_base +
	    KSTACK_SIZE;
	if (old->user)
		fxsave(old->pcb.fpu);
	if (next->user)
		fxrstor(next->pcb.fpu);
	kep_amd64_asm_switch(&old->pcb, &next->pcb);
}

void
kep_arch_thread_init(kthread_t *thread, void *stack_base,
    struct karch_trapframe *forkframe, void (*func)(void *), void *arg)
{
	uint64_t *sp;

	memset(&thread->pcb, 0x0, sizeof(thread->pcb));

	if (forkframe == NULL) {
		sp = thread->kstack_base + KSTACK_SIZE;
	} else {
		karch_trapframe_t *frame;

		sp = thread->kstack_base + KSTACK_SIZE - sizeof(*forkframe);
		memcpy(sp, forkframe, sizeof(*forkframe));

		frame = (karch_trapframe_t *)sp;
		frame->rax = 0; /* fork returns 0 in child */
	}

	thread->pcb.rdi = (uint64_t)func;
	thread->pcb.rsi = (uint64_t)arg;

	sp -= 2;
	*sp = (uint64_t)kep_amd64_asm_thread_trampoline;;
	thread->pcb.rsp = (uint64_t)(sp);
}
