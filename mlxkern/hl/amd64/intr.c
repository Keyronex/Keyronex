/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */

#include <bsdqueue/queue.h>

#include "amd64.h"
#include "asmintr.h"
#include "hl/hl.h"
#include "ke/ke.h"
#include "vm/vm.h"

typedef struct {
	uint16_t isr_low;
	uint16_t selector;
	uint8_t ist;
	uint8_t type;
	uint16_t isr_mid;
	uint32_t isr_high;
	uint32_t zero;
} __attribute__((packed)) idt_entry_t;

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
}

void
handle_int(hl_intr_frame_t *frame, uintptr_t num)
{
	ipl_t ipl;
	struct md_intr_entry *entry;

	if (num == kIntNumSwitch) {
		/* here the context switch actually happens */
		kthread_t *old = hl_curcpu()->hl.oldthread,
			  *next = hl_curcpu()->hl.newthread;

#if DEBUG_SCHED == 1
		ke_dbg("Switch from %p to %p\n", old, next);
#endif

		old->frame = *frame;
		*frame = next->frame;

		hl_curcpu()->hl.tss->rsp0 = next->kstack;

		ke_spinlock_release_nospl(&dispatcher_lock);
		splx(next->saved_ipl);
		return;
	}

	kdprintf("Unhandled interrupt %lu. CR2: 0x%lx\n", num, read_cr2());
	md_intr_frame_trace(frame);
	kfatal("Halting.\n");
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
hl_switch(struct kthread *from, struct kthread *to)
{
	ke_curthread()->saved_ipl = splget();
	hl_curcpu()->hl.oldthread = from;
	hl_curcpu()->hl.newthread = to;
	from->hl.fs = rdmsr(kAMD64MSRFSBase);
	wrmsr(kAMD64MSRFSBase, to->hl.fs);
	/* the dispatcher db lock will be dropped (and IPL too) here */
	asm("int $240");
}
