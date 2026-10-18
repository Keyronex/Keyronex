/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file aspace.c
 * @brief amd64 address space loading and TLB invalidation.
 */

#include <sys/k_aspace.h>
#include <sys/vm.h>

#define CR4_PGE (1ul << 7)

void
kep_arch_aspace_load(paddr_t root)
{
	asm volatile("mov %0, %%cr3" : : "r"(root) : "memory");
}

void
kep_arch_tlb_invalidate(const struct ke_aspace *as, vaddr_t va, size_t npages,
    unsigned flags)
{
	(void)flags;

	if (npages == 0) {
		unsigned long cr;

		if (as->kernel) {
			asm volatile("mov %%cr4, %0" : "=r"(cr));
			asm volatile("mov %0, %%cr4"
			    :
			    : "r"(cr & ~CR4_PGE)
			    : "memory");
			asm volatile("mov %0, %%cr4" : : "r"(cr) : "memory");
		}

		asm volatile("mov %%cr3, %0\n\t"
			     "mov %0, %%cr3\n\t"
		    : "=r"(cr)
		    :
		    : "memory");

		return;
	}

	for (size_t i = 0; i < npages; i++)
		asm volatile("invlpg (%0)"
		    :
		    : "r"(va + (i << PGSHIFT))
		    : "memory");
}

bool
kep_arch_tlb_invalidate_remote(const struct ke_aspace *as, vaddr_t va,
    size_t npages, unsigned flags)
{
	(void)as; (void)va; (void)npages; (void)flags;
	return false;
}
