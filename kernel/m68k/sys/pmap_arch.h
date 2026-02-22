/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file pmap_arch.h
 * @brief m68k physical map definitions.
 */

#ifndef ECX_KEYRONEX_PMAP_ARCH_H
#define ECX_KEYRONEX_PMAP_ARCH_H

#include <sys/vm_arch.h>
#include <sys/k_log.h>

#include <stdbool.h>
#include <stdint.h>

#define PMAP_ROOTLEVEL_SPAN (32ul * 1024 * 1024 * 8) /* 32mib * 8 PTEs */

/*
 * we assume 68040 using 4kib pages.
 *
 * the format:
 * top 7 bits = pml3 entry (root index field RI)
 * next 7 = pml2 entry (pointer index field PI)
 * next 6 = pml1 entry (page index field PGI)
 * last 12 = page offset
 *
 * in pml3, there are 128 root-level table descriptors;
 * in pml2, 128 pointer-level table descriptors;
 * in pml1, 64 page descriptors in the page-level table (pml1).
 */

union vaddr_040 {
	struct {
		uint32_t l2i : 7, l1i : 7, l0i : 6, pgi : 12;
	};
	uint32_t addr;
};

/*
 * Since we allocate a whole page's worth of PML0 and PML1 at once, they can be
 * treated as containing 1024 PTEs for the purposes of the skip logic that uses
 * these definitions.
 */
#define PMAP_L1_SKIP 64
#define PMAP_L0_SKIP 1024

/* root table descriptor */
typedef struct __attribute__((packed))  pml2e_040 {
	uint32_t addr : 28, used : 1, writeprotect : 1, type : 2;
} pml2e_040_t;

/* pointer table descriptor */
typedef struct __attribute__((packed))  pml1e_040 {
	uint32_t addr : 28, used : 1, writeprotect : 1, type : 2;
} pml1e_040_t;

/* 4k page-level page descriptor */
typedef struct  __attribute__((packed)) pml0e_040 {
	uint32_t pfn : 20, userreserved : 1, global : 1, user1 : 1, user0 : 1,
	    supervisor : 1, cachemode : 2, modified : 1, used : 1,
	    writeprotect : 1, type : 2;
} pml0e_040_t;

_Static_assert(sizeof(pml2e_040_t) == sizeof(uint32_t), "Bad pml3e size");
_Static_assert(sizeof(pml1e_040_t) == sizeof(uint32_t), "Bad pml2e size");
_Static_assert(sizeof(pml0e_040_t) == sizeof(uint32_t), "Bad pml1e size");

typedef enum pmap_level {
	PMAP_L0,
	PMAP_L1,
	PMAP_L2,
} pmap_level_t;

struct pte_soft {
	uintptr_t kind: 3, data: 27, hw_type: 2;
};

typedef union pte {
	struct pte_soft soft;
	pml2e_040_t hw_pml2_040;
	pml1e_040_t hw_pml1_040;
	pml0e_040_t hw_pml0_040;
	uintptr_t value;
} pte_t;

static inline void
pmap_indexes(vaddr_t vaddr, size_t indexes[PMAP_MAX_LEVELS])
{
	union vaddr_040 addr;
	addr.addr = vaddr;
	indexes[0] = addr.l0i;
	indexes[1] = addr.l1i;
	indexes[2] = addr.l2i;
}

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
	return pte.hw_pml0_040.type != 0;
}

static inline bool
pmap_pte_hw_is_large(pte_t pte, size_t level)
{
	return false;
}

static inline pte_t
pmap_pte_hwleaf_create(pte_t *ppte, uintptr_t pfn, pmap_level_t level,
    vm_prot_t prot, vm_cache_mode_t mode)
{
	union pte pte = {
		.hw_pml0_040 = {
			.pfn = pfn,
			.cachemode = 1,
			.supervisor = (prot & VM_USER) ? 0 : 1,
			.global = (pfn << 12) >= HIGHER_HALF,
			.writeprotect = (prot & VM_WRITE) ? 0 : 1,
			.type = 1 /* resident*/
		}
	};

	pmap_store_pte(ppte, pte);

	return pte;
}

static inline paddr_t
pmap_pte_hwleaf_paddr(pte_t pte, pmap_level_t level)
{
	kassert(level == PMAP_L0);
	return pte.hw_pml0_040.pfn << 12;
}


static inline bool
pmap_pte_hwleaf_executable(pte_t pte)
{
	return true;
}

static inline bool
pmap_pte_hwleaf_writeable(pte_t pte)
{
	return pte.hw_pml0_040.writeprotect == 0;
}

static inline void
pmap_pte_hwleaf_set_writeable(pte_t *ppte)
{
	pte_t pte = pmap_load_pte(ppte);
	pte.hw_pml0_040.writeprotect = 0;
	pmap_store_pte(ppte, pte);
}


static inline void
pmap_pte_hwleaf_clear_writeable(pte_t *ppte)
{
	pte_t pte = pmap_load_pte(ppte);
	pte.hw_pml0_040.writeprotect = 1;
	pmap_store_pte(ppte, pte);
}

static inline void
pmap_pte_soft_create(pte_t *ppte, int kind, uintptr_t data, bool was_hw)
{
	kassert(data <= 0x7FFFFFF);
	union pte pte = {
		.soft = {
			.hw_type = 0,
			.data = data,
			.kind = kind,
		},
	};
	pmap_store_pte(ppte, pte);
}

static inline void
pmap_pte_zeroleaf_create(pte_t *ppte, pmap_level_t)
{
	pmap_store_pte(ppte, (union pte){.value = 0});
}

static inline size_t
m68040_dir_nptes_group(pmap_level_t level)
{
	switch(level){
		case PMAP_L2:
			return 8;
		case PMAP_L1:
			return 16;
		default:
			kfatal("m68040_dir_nptes_group: bad level");
	}
}

/*
 * Since m68k page tables are sub-pagesize, but the kernel expects to deal in
 * page-sized page tables, in the page directories we deal in groups of PTEs
 * that make 1 PGSIZE worth of the next-level-lower tables.
 */

static inline pte_t
pmap_pte_hwdir_create(pte_t *ppte, paddr_t table, pmap_level_t level)
{
	size_t nptes = m68040_dir_nptes_group(level);
	size_t size = PGSIZE / nptes;
	uintptr_t mask = (nptes * sizeof(pte_t)) - 1;
	pte_t *start_ppte = (pte_t*)((uintptr_t)ppte & ~mask);
	pte_t ret;

	kassert((table & (PGSIZE - 1)) == 0);

	for (size_t i = 0; i < nptes; i++) {
		union pte pte;
		paddr_t paddr;

		paddr = table + (i * size);

		if (level == PMAP_L2) {
			pte.hw_pml2_040.addr = paddr >> 4;
			pte.hw_pml2_040.used = 0;
			pte.hw_pml2_040.writeprotect = 0;
			pte.hw_pml2_040.type = 2;
		} else {
			pte.hw_pml1_040.addr = paddr >> 4;
			pte.hw_pml1_040.used = 0;
			pte.hw_pml1_040.writeprotect = 0;
			pte.hw_pml1_040.type = 2;
		}

		pmap_store_pte(start_ppte + i, pte);

		if ((start_ppte + i) == ppte)
			ret = pte;
	}

	return ret;
}

static inline void
pmap_pte_softdir_create(pte_t *ppte, pmap_level_t level, int kind,
    uintptr_t data, bool was_hw)
{
	size_t nptes = m68040_dir_nptes_group(level);
	uintptr_t mask = (nptes * sizeof(pte_t)) - 1;
	pte_t *start_ppte = (pte_t*)((uintptr_t)ppte & ~mask);
	union pte pte = {
		.soft = {
			.hw_type = 0,
			.data = data,
			.kind = kind,
		},
	};

	for (size_t i = 0; i < nptes; i++)
		pmap_store_pte(start_ppte + i, pte);
}

static inline void
pmap_pte_zerodir_create(pte_t *ppte, pmap_level_t level)
{
	size_t nptes = m68040_dir_nptes_group(level);
	uintptr_t mask = (nptes * sizeof(pte_t)) - 1;
	pte_t *start_ppte = (pte_t*)((uintptr_t)ppte & ~mask);

	for (size_t i = 0; i < nptes; i++) {
		union pte pte;
		pte.value = 0;
		pmap_store_pte(start_ppte + i, pte);
	}
}

static inline paddr_t
pmap_pte_hwdir_paddr(pte_t pte, int level)
{
	switch (level) {
	case PMAP_L2:
		return pte.hw_pml2_040.addr << 4;
	case PMAP_L1:
		return pte.hw_pml1_040.addr << 4;
	default:
		kfatal("unexpected level");
	}
}

#endif /* ECX_KEYRONEX_PMAP_ARCH_H */
