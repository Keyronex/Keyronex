/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */

#include <bsdqueue/queue.h>

#include "amd64.h"
#include "asmintr.h"
#include "kdk/amd64/mdamd64.h"
#include "kdk/kernel.h"
#include "kdk/machdep.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "posix/pxp.h"

typedef struct {
	uint16_t isr_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t type;
	uint16_t isr_mid;
	uint32_t isr_high;
	uint32_t zero;
} __attribute__((packed)) idt_entry_t;

static bool syscall(hl_intr_frame_t *frame, void *arg);
static bool double_fault(hl_intr_frame_t *frame, void *arg);
static bool page_fault(hl_intr_frame_t *frame, void *arg);

static TAILQ_HEAD(intr_entries, intr_entry) intr_entries[256];
static idt_entry_t idt[256] = { 0 };

void
idt_load(void)
{
	struct {
		uint16_t limit;
		vaddr_t addr;
	} __attribute__((packed)) idtr = { sizeof(idt) - 1, (vaddr_t)idt };

	asm volatile("lidt %0" : : "m"(idtr));
}

static void
idt_set(uint8_t index, vaddr_t isr, uint8_t type, uint8_t ist)
{
	idt[index].isr_low = (uint64_t)isr & 0xFFFF;
	idt[index].isr_mid = ((uint64_t)isr >> 16) & 0xFFFF;
	idt[index].isr_high = (uint64_t)isr >> 32;
	idt[index].selector = 0x28; /* sixth */
	idt[index].type = type;
	idt[index].ist = ist;
	idt[index].zero = 0x0;
}

static struct intr_entry pagefault_intr_entry, doublefault_intr_entry,
    syscall_entry, hardclock_intr_entry, reschedule_ipi_intr_entry;

/* setup the initial IDT */
void
idt_setup(void)
{
#define IDT_SET(VAL) idt_set(VAL, (vaddr_t)&isr_thunk_##VAL, INT, 0);
	NORMAL_INTS(IDT_SET);
#undef IDT_SET

#define IDT_SET(VAL, GATE) idt_set(VAL, (vaddr_t)&isr_thunk_##VAL, GATE, 0);
	SPECIAL_INTS(IDT_SET);
#undef IDT_SET

	idt_load();

	for (int i = 0; i < elementsof(intr_entries); i++) {
		TAILQ_INIT(&intr_entries[i]);
	}
	md_intr_register("double-fault", 8, kIPLHigh, double_fault, NULL, false,
	    &doublefault_intr_entry);
	md_intr_register("page-fault", 14, kIPLAPC, page_fault, NULL, false,
	    &pagefault_intr_entry);
	md_intr_register("reschedule-ipi", kIntNumRescheduleIPI, kIPLHigh,
	    ki_reschedule_ipi, NULL, false, &reschedule_ipi_intr_entry);
	md_intr_register("hardclock", kIntNumLAPICTimer, kIPLHigh,
	    ki_cpu_hardclock, NULL, false, &hardclock_intr_entry);

	md_intr_register("syscall", kIntNumSyscall, kIPL0, syscall, NULL, false,
	    &syscall_entry);
}

#define DEBUG_SCHED 0

static void
hang_until_told(void)
{
	volatile bool leave = false;
	while (!leave)
		;
}

void
handle_int(hl_intr_frame_t *frame, uintptr_t num)
{
	ipl_t ipl = splget();
	struct intr_entries *entries;
	struct intr_entry *entry;

	if (num == kIntNumSwitch) {
		/* here the context switch actually happens */
		kthread_t *old = hl_curcpu()->hl.oldthread,
			  *next = hl_curcpu()->hl.newthread;

#if DEBUG_SCHED == 1
		kdprintf("Switch from %p to %p\n", old, next);
#endif

		if (old->state == kThreadStateDone) {
			/* we can now put this on the to-free list */
			TAILQ_INSERT_TAIL(&hl_curcpu()->thread_free_queue, old,
			    runqueue_link);
			ke_dpc_enqueue(&hl_curcpu()->thread_free_dpc);
		} else {
			old->frame = *frame;
		}

		*frame = next->frame;
		hl_curcpu()->hl.tss->rsp0 = next->kstack;

		ke_spinlock_release_nospl(&dispatcher_lock);
		ipl = next->saved_ipl;
		goto ast;
	} else if (num == kIntNumIPIInvlPG) {
		void pmap_global_invlpg_cb(void);
		pmap_global_invlpg_cb();
		goto out;
	}

	entries = &intr_entries[num];

	if (TAILQ_EMPTY(entries)) {
		kdprintf("Unhandled interrupt %lu. CR2: 0x%lx\n", num,
		    read_cr2());
		md_intr_frame_trace(frame);
		hang_until_told();
		goto out;
	}

	TAILQ_FOREACH (entry, entries, queue_entry) {
		ipl = MAX2(ipl, entry->ipl);
	}

	if (ipl < splget()) {
		kdprintf(
		    "In handling interrupt %lu (cr2: 0x%lx):\n"
		    "IPL not less or equal (running at %d, interrupt priority %d)\n",
		    num, read_cr2(), splget(), ipl);
		md_intr_frame_trace(frame);
		hang_until_told();
		goto out;
	}
	ipl = splraise(ipl);

	if (num == 14) {
		void *arg = (void *)read_cr2();
		/* we need cr2 *before* we go to the page fault */
		asm("sti");
		page_fault(frame, arg);
	} else {
		asm("sti");
		TAILQ_FOREACH (entry, entries, queue_entry) {
			bool r = entry->handler(frame, entry->arg);
			(void)r;
		}
	}

	asm("cli");
out:
	if (num >= 32) {
		void lapic_eoi(void);
		lapic_eoi();
	}

ast:
	/* invoke usermode AST if indicated */
	if (frame->cs & 0x3)
		pxp_ast(frame);

	splx(ipl);
}

static __attribute__((noreturn)) bool
double_fault(hl_intr_frame_t *frame, void *arg)
{
	kdprintf("double fault\n");
	for (;;)
		;
}

static bool
page_fault(hl_intr_frame_t *frame, void *arg)
{
	vm_fault_return_t ret;
	vaddr_t vaddr = (uintptr_t)arg & ~(PGSIZE - 1);
	ethread_t *thr = ps_curthread();

	while (true) {
		if (thr->in_pagefault) {
			md_intr_frame_trace(frame);
			kdprintf("vm_fault: nested fault\n");
			for (;;)
				;
		}
		thr->in_pagefault = true;
		ret = vm_fault(ps_curproc()->map, vaddr, frame->code, NULL);
		thr->in_pagefault = false;

		switch (ret) {
		case kVMFaultRetOK:
			return true;

		case kVMFaultRetRetry:
			continue;

		case kVMFaultRetFailure:
			md_intr_frame_trace(frame);

			if (frame->cs & 0x3)
				kdprintf(
				    "vm_fault failed in userland (proc %u)",
				    ps_curproc()->id);
			kdprintf("vm_fault failed");

			hang_until_told();
			return kVMFaultRetOK;

		case kVMFaultRetPageShortage:
			kfatal("path unimplemented\n");
		}
	}
	return true;
}

static bool
syscall(hl_intr_frame_t *frame, void *arg)
{
	int posix_syscall(hl_intr_frame_t * frame);
	posix_syscall(frame);
	return true;
}

int
md_intr_alloc(const char *name, ipl_t prio, intr_handler_t handler, void *arg,
    bool shareable, uint8_t *vector, struct intr_entry *entry)
{
	/* first vector appropriate to the priority */
	uint8_t starting = MAX2(prio << 4, 32);

	for (int i = starting; i < starting + 16; i++) {
		struct intr_entry *slot = TAILQ_FIRST(&intr_entries[i]);
		if (slot == NULL || (slot->shareable && shareable)) {
			md_intr_register(name, i, prio, handler, arg, shareable,
			    entry);
			if (vector)
				*vector = i;
			return 0;
		}
	}

	return -1;
}

void
md_intr_register(const char *name, uint8_t vec, ipl_t prio,
    intr_handler_t handler, void *arg, bool shareable, struct intr_entry *entry)
{
	entry->name = name;
	entry->ipl = prio;
	entry->handler = handler;
	entry->arg = arg;
	entry->shareable = shareable;
	TAILQ_INSERT_TAIL(&intr_entries[vec], entry, queue_entry);
}

void
md_intr_frame_trace(hl_intr_frame_t *frame)
{
	struct frame {
		struct frame *rbp;
		uint64_t rip;
	} *aframe = (struct frame *)frame->rbp;
	const char *name = NULL;
	size_t offs = 0;

	// ksrv_backtrace((vaddr_t)frame->rip, &name, &offs);
	kdprintf("Begin stack trace:\n");
	kdprintf(" - %p %s+%lu\n", (void *)frame->rip, name ? name : "???",
	    offs);

	if (aframe != NULL)
		do {
			name = NULL;
			offs = 0;
			// ksrv_backtrace((vaddr_t)aframe->rip, &name, &offs);
			kdprintf(" - %p %s+%lu\n", (void *)aframe->rip,
			    name ? name : "???", offs);
		} while ((aframe = aframe->rbp) &&
		    (uint64_t)aframe >= 0xffff80000000 && aframe->rip != 0x0);
}

void
hl_switch(struct kthread *from, struct kthread *to, bool drop)
{
	from->saved_ipl = splget();
	hl_curcpu()->hl.oldthread = from;
	hl_curcpu()->hl.newthread = to;
	from->hl.fs = rdmsr(kAMD64MSRFSBase);
	wrmsr(kAMD64MSRFSBase, to->hl.fs);

	/* the dispatcher db lock will be dropped (and IPL too) in the ISR */

	/* switch stack if thread is going away */
	if (drop)
		asm volatile("mov %0, %%rsp; int $240" : : "r"(to->frame.rsp));

	asm volatile("int $240");
}
