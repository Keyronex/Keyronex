/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm_arch.h
 * @brief AMD64 physical map definitions.
 */

#ifndef ECX_KEYRONEX_PMAP_ARCH_H
#define ECX_KEYRONEX_PMAP_ARCH_H

#include <sys/vm_arch.h>

#include <stdbool.h>
#include <stdint.h>

#define PMAP_ROOTLEVEL_SPAN (1ULL << 39) /* 512gib */

typedef enum pmap_level {
	PMAP_L0,
	PMAP_L1,
	PMAP_L2,
	PMAP_L3,
} pmap_level_t;

struct pte_hw_large {
	uint64_t valid:	1,
	    writeable:	1,
	    user:	1,
	    pwt:	1,
	    pcd:	1,
	    accessed:	1,
	    dirty:	1,
	    ps:		1,
	    global:	1,
	    age:	2, /* uses part of the available bits */
	    available1:	1,
	    pat:	1,
	    pfn:	39,
	    available2: 11,
	    nx:		1;
} ;

struct pte_hw_l0 {
	uint64_t valid:	1,
	    writeable:	1,
	    user:	1,
	    pwt:	1,
	    pcd:	1,
	    accessed:	1,
	    dirty:	1,
	    pat:	1,
	    global:	1,
	    age:	2, /* uses part of the available bits */
	    available1:	1,
	    pfn:	40,
	    available2:	11,
	    nx:		1;
};

struct pte_soft {
	uint64_t valid : 1, data : 60, kind : 3;
};

typedef union pte {
	uintptr_t value;
	struct pte_hw_l0 hw;
	struct pte_hw_large hw_large;
	struct pte_soft soft;
} pte_t;

static inline union pte
pmap_load_pte(pte_t *ppte)
{
	union pte pte;
	__atomic_load(ppte, &pte, __ATOMIC_RELAXED);
	return pte;
}

static inline void
pmap_store_pte(pte_t *ppte, union pte pte)
{
	__atomic_store(ppte, &pte, __ATOMIC_RELAXED);
}

static inline bool
pmap_pte_is_hw(pte_t pte)
{
	return pte.hw.valid;
}

static inline bool
pmap_pte_hw_is_large(pte_t pte, pmap_level_t level)
{
	return pte.hw_large.valid && pte.hw_large.ps;
}

static inline pte_t
pmap_pte_hwleaf_create(pte_t *ppte, uintptr_t pfn, pmap_level_t level,
    vm_prot_t prot, vm_cache_mode_t mode)
{
	union pte pte;

	if (level == 0)
		pte = (union pte) {
			.hw = {
				.valid = 1,
				.writeable = (prot & VM_WRITE) != 0,
				.user = (prot & VM_USER) != 0,
				.pwt = mode & 1,
				.pcd = mode & 2,
				.accessed = 1,
				.dirty = 0,
				.pat = mode & 4,
				.global = (prot & VM_USER) == 0,
				.available1 = 0,
				.pfn = pfn,
				.available2 = 0,
				.nx = (prot & VM_EXEC) == 0,
			},
		};
	else
		pte = (union pte) {
			.hw_large = {
				.valid = 1,
				.writeable = (prot & VM_WRITE) != 0,
				.user = (prot & VM_USER) != 0,
				.pwt = mode & 1,
				.pcd = mode & 2,
				.accessed = 1,
				.dirty = 0,
				.ps = 1,
				.global = (prot & VM_USER) == 0,
				.available1 = 0,
				.pat = mode & 4,
				.pfn = pfn >> 1,
				.available2 = 0,
				.nx = (prot & VM_EXEC) == 0,
			},
		};

	pmap_store_pte(ppte, pte);

	return pte;
}

static inline paddr_t
pmap_pte_hwleaf_paddr(pte_t pte, pmap_level_t level)
{
	return (level == PMAP_L0 ? pte.hw.pfn : pte.hw_large.pfn) << PGSHIFT;
}

static inline void
pmap_pte_zeroleaf_create(pte_t *ppte, pmap_level_t level)
{
	union pte pte = { .hw = { .valid = 0 } };
	pmap_store_pte(ppte, pte);
}

static inline pte_t
pmap_pte_hwdir_create(pte_t *ppte, paddr_t table, pmap_level_t level)
{
	uintptr_t pfn = table >> PGSHIFT;
	union pte pte = {
		.hw = {
			.valid = 1,
			.writeable = 1,
			.user = 1,
			.pwt = 0,
			.pcd = 0,
			.accessed = 0,
			.dirty = 0,
			.pat = 0,
			.global = 0,
			.available1 = 0,
			.pfn = pfn,
			.available2 = 0,
			.nx = 0,
		},
	};
	pmap_store_pte(ppte, pte);
	return pte;
}

static inline void
pmap_pte_zerodir_create(pte_t *ppte, pmap_level_t level)
{
	union pte pte = { .hw = { .valid = 0 } };
	pmap_store_pte(ppte, pte);
}

static inline paddr_t
pmap_pte_hwdir_paddr(pte_t pte, pmap_level_t level)
{
	return pte.hw.pfn << PGSHIFT;
}

#endif /* ECX_KEYRONEX_PMAP_ARCH_H */
