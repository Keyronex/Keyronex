/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dlog.c
 * @brief AMD64 interrupt handling.
 */

#include <keyronex/cpu.h>
#include <keyronex/dlog.h>
#include <keyronex/intr.h>
#include <keyronex/pcb.h>
#include <keyronex/x86.h>

#include "asm_intr.h"

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

void kep_dispatch_softints(ipl_t newipl);

static struct idt_entry bsp_idt[256];

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
	}

	asm volatile("lidt %0" : : "m"(idtr));
}

void
kep_amd64_interrupt(karch_trapframe_t *frame, uintptr_t num)
{
	kfatal("interrupt %d\n", num);
}

void
kep_arch_switch(struct kthread *old, struct kthread *new)
{
	kfatal("kep_arch_switch\n");
}
