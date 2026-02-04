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

void kep_dispatch_softints(ipl_t newipl);

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
