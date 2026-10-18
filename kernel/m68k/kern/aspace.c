/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file aspace.c
 * @brief m68k address space loading and ATC invalidation.
 */

#include <sys/k_aspace.h>
#include <sys/vm.h>

void
kep_arch_aspace_load(paddr_t root)
{
	asm volatile("movec %0, %%urp\n\t"
		     "movec %0, %%srp\n\t"
		     "pflusha\n\t"
	    :
	    : "r"(root)
	    : "memory");
}

void
kep_arch_tlb_invalidate(const struct ke_aspace *as, vaddr_t va, size_t npages,
    unsigned flags)
{
	(void)as;

	if (npages == 0 || (flags & KE_INVAL_NONLEAF)) {
		asm volatile("pflusha" : : : "memory");
		return;
	}

	for (size_t i = 0; i < npages; i++)
		asm volatile("pflush (%0)" : : "a"(va + (i << PGSHIFT))
		    : "memory");
}

bool
kep_arch_tlb_invalidate_remote(const struct ke_aspace *as, vaddr_t va,
    size_t npages, unsigned flags)
{
	(void)as; (void)va; (void)npages; (void)flags;
	return false;
}
