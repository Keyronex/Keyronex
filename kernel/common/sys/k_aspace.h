/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file k_aspace.h
 * @brief Kernel address space structure, operations, and per-arch operations.
 */

#ifndef ECX_KEYRONEX_K_ASPACE_H
#define ECX_KEYRONEX_K_ASPACE_H

#include <sys/k_cpu.h>

struct ke_aspace {
	paddr_t root;	/* (~) the root page table */
	bool kernel;	/* (~) whether this is the kernel map's aspace */
	katomic_cpumask_t active_cpus; /* CPUs that may be on this aspace. */
	atomic_uint invalidate_gen; /* invalidation generation */
};

enum ke_aspace_inval_flags {
	KE_INVAL_LEAF = 0,	/* only leaf translations in range */
	KE_INVAL_NONLEAF = 1,	/* directory entry also changed */
};

void ke_aspace_init(struct ke_aspace *as, paddr_t root, bool kernel);
void ke_aspace_activate(struct ke_aspace *old, struct ke_aspace *new);
void ke_aspace_invalidate(struct ke_aspace *as, vaddr_t va, size_t len,
    unsigned flags);
void ke_aspace_invalidate_all(struct ke_aspace *as, unsigned flags);

static inline unsigned
ke_aspace_gen(const struct ke_aspace *as)
{
	return atomic_load_explicit(&as->invalidate_gen, memory_order_acquire);
}

/* per-arch operations */

/* Switch this CPU to the given root page table.*/
void kep_arch_aspace_load(paddr_t root);

/*
 * Locally invalidate TLB from va to va + npages * PGSIZE.
 * If npages is 0, invalidate everything.
 */
void kep_arch_tlb_invalidate(const struct ke_aspace *as, vaddr_t va,
    size_t npages, unsigned flags);

/*
 * As kep_arch_tlb_invalidate, but invalidate on all remote CPUs.
 *
 * Returns true if the architecture did its own broadcast, false if not and the
 * caller should do an xcall instead.
 */
bool kep_arch_tlb_invalidate_remote(const struct ke_aspace *as, vaddr_t va,
    size_t npages, unsigned flags);

#endif /* ECX_KEYRONEX_K_ASPACE_H */
