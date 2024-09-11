/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#ifndef KRX_RISCV64_VMP_RISCV64_H
#define KRX_RISCV64_VMP_RISCV64_H

#include <stdint.h>

#include "kdk/vm.h"
#include "kern/ki.h"

#define VMP_TABLE_LEVELS 4
#define VMP_PAGE_SHIFT 12

#define VMP_LEVEL_4_ENTRIES 512
#define VMP_LEVEL_3_ENTRIES 512
#define VMP_LEVEL_2_ENTRIES 512
#define VMP_LEVEL_1_ENTRIES 512

#define VMP_LEVEL_4_ENTRIES 512
#define VMP_LEVEL_3_ENTRIES 512
#define VMP_LEVEL_2_ENTRIES 512
#define VMP_LEVEL_1_ENTRIES 512

struct vmp_forkpage;

enum software_pte_kind {
	/*! PTE represents an address in swap. */
	kSoftPteKindSwap,
	/*! PTE represents a page being read-in from disk. */
	kSoftPteKindBusy,
	/*! PTE is transitional between memory and disk; not in working set. */
	kSoftPteKindTrans,
	kSoftPteKindFork,
};

struct vmp_md_procstate {
	/*! physical address of the translation table */
	paddr_t table;
};

typedef struct __attribute__((packed)) pte_sw {
	uint64_t valid : 1, /* must = 0*/
	    data : 61;	    /* pfn, swap descriptor, fork pointer */
	enum software_pte_kind kind : 2;
} pte_sw_t;

typedef struct __attribute__((packed)) pte_hw {
	uint64_t valid : 1, read : 1, write : 1, execute : 1, user : 1,
	    global : 1, accessed : 1, dirty : 1, rsw : 2, pfn : 44,
	    reserved : 7, pbmt : 2, n : 1;
} pte_hw_t;

typedef pte_hw_t pte_hwl2_t;
typedef pte_hw_t pte_hwl3_t;

typedef union __attribute__((packed)) pte {
	pte_hw_t hw;
	pte_hwl2_t hwl2;
	pte_hwl3_t hwl3;
	pte_sw_t sw;
	uintptr_t value;
} pte_t;

static inline void
vmp_md_pte_create_hw(pte_t *ppte, pfn_t pfn, bool writeable, bool executable,
    bool cacheable, bool user)
{
	pte_t pte;

	pte.value = 0x0;
	pte.hw.pfn = pfn;
	pte.hw.valid = 1;
	pte.hw.read = pte.hw.accessed = 1;
	pte.hw.write = pte.hw.dirty = writeable ? 1 : 0;
	pte.hw.execute = executable ? 1 : 0;
	if (user)
		pte.hw.user = 1;
#if 0 /* not supported by qemu? */
	pte.hw.pbmt = cacheable ? 0b00 : 0b10; /* PMA : IO */
#endif
	__atomic_store(ppte, &pte, __ATOMIC_RELAXED);
	asm volatile("fence\n\t" ::: "memory");
}

#define vmp_md_pte_create_hwl2 vmp_md_pte_create_hw
#define vmp_md_pte_create_hwl3 vmp_md_pte_create_hw

static inline void
vmp_md_pte_create_busy(pte_t *ppte, pfn_t pfn)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = pfn;
	pte.sw.kind = kSoftPteKindBusy;
	ppte->value = pte.value;
	__atomic_store(ppte, &pte, __ATOMIC_RELAXED);
}

static inline void
vmp_md_pte_create_trans(pte_t *ppte, pfn_t pfn)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = pfn;
	pte.sw.kind = kSoftPteKindTrans;
	__atomic_store(ppte, &pte, __ATOMIC_RELAXED);
	asm volatile("fence\n\t" ::: "memory");
}

static inline void
vmp_md_pte_create_swap(pte_t *ppte, pfn_t pfn)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = pfn;
	pte.sw.kind = kSoftPteKindSwap;
	__atomic_store(ppte, &pte, __ATOMIC_RELAXED);
	/* no barrier - swap PTEs always replace trans PTEs. */
}

static inline void
vmp_md_pte_create_fork(pte_t *ppte, struct vmp_forkpage *forkpage)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = (uintptr_t)forkpage >> 3;
	pte.sw.kind = kSoftPteKindFork;
	ppte->value = pte.value;
}

static inline void
vmp_md_pte_create_zero(pte_t *ppte)
{
	pte_t pte;
	pte.value = 0;
	__atomic_store(ppte, &pte, __ATOMIC_RELAXED);
	asm volatile("fence\n\t" ::: "memory");
}

static inline bool
vmp_md_pte_is_empty(pte_t *pte)
{
	return pte->value == 0;
}

static inline bool
vmp_md_pte_is_valid(pte_t *pte)
{
	return pte->hw.valid;
}

static inline bool
vmp_md_pte_hw_is_large(pte_t *pte, int level)
{
	kassert(level > 1);
	return pte->hw.valid &&
	    (pte->hw.read || pte->hw.write || pte->hw.execute);
}

static inline bool
vmp_md_hw_pte_is_writeable(pte_t *pte)
{
	return pte->hw.write == 1;
}

static inline void
vmp_md_pte_hw_set_readonly(pte_t *pte)
{
	pte->hw.write = 0;
}

static inline enum vmp_pte_kind
vmp_pte_characterise(pte_t *pte)
{
	if (vmp_md_pte_is_empty(pte))
		return kPTEKindZero;
	else if (vmp_md_pte_is_valid(pte))
		return kPTEKindValid;
	else if (pte->sw.kind == kSoftPteKindBusy)
		return kPTEKindBusy;
	else if (pte->sw.kind == kSoftPteKindTrans)
		return kPTEKindTrans;
	else if (pte->sw.kind == kSoftPteKindFork)
		return kPTEKindFork;
	else {
		kassert(pte->sw.kind == kSoftPteKindSwap);
		return kPTEKindSwap;
	}
}

static inline pfn_t
vmp_md_soft_pte_pfn(pte_t *pte)
{
	kassert(vmp_pte_characterise(pte) == kPTEKindBusy ||
	    vmp_pte_characterise(pte) == kPTEKindTrans ||
	    vmp_pte_characterise(pte) == kPTEKindSwap);
	return pte->sw.data;
}

static inline struct vmp_forkpage *
vmp_md_soft_pte_forkpage(pte_t *pte)
{
	kassert(vmp_pte_characterise(pte) == kPTEKindFork);
	return (struct vmp_forkpage *)((uintptr_t)pte->sw.data << 3);
}

/* new stuff */
static inline pfn_t
vmp_md_pte_hw_pfn(pte_t *pte, int level)
{
	return pte->hw.pfn;
}

static inline paddr_t
vmp_pte_hw_paddr(pte_t *pte, int level)
{
	return (paddr_t)pte->hw.pfn << VMP_PAGE_SHIFT;
}

static inline void
vmp_addr_unpack(vaddr_t vaddr, int unpacked[VMP_TABLE_LEVELS + 1])
{
	uintptr_t virta = (uintptr_t)vaddr;
	unpacked[4] = ((virta >> 39) & 0x1FF);
	unpacked[3] = ((virta >> 30) & 0x1FF);
	unpacked[2] = ((virta >> 21) & 0x1FF);
	unpacked[1] = ((virta >> 12) & 0x1FF);
}

static inline void
vmp_page_dcache_flush_pre_readin(vm_page_t *page)
{
	//vaddr_t base = vm_page_direct_map_addr(page);
	//ki_dcache_invalidate_range(base, base + PGSIZE);
}

/* deals with speculative loads */
static inline void
vmp_page_dcache_flush_post_readin(vm_page_t *page)
{
	//vaddr_t base = vm_page_direct_map_addr(page);
	//ki_dcache_invalidate_range(base, base + PGSIZE);
}

static inline void
vmp_page_flush_dcache(vm_page_t *page)
{
	//vaddr_t base = vm_page_direct_map_addr(page);
	//ki_dcache_clean_invalidate_range(base, base + PGSIZE);
}

static inline void
vmp_page_sync_icache(vm_page_t *page)
{
	//vaddr_t base = vm_page_direct_map_addr(page);
	//ki_icache_synchronise_range(base, base + PGSIZE);
}

static inline void
vmp_page_clean_dcache_postevict(vm_page_t *page)
{
	//vaddr_t base = vm_page_direct_map_addr(page);
	//ki_dcache_clean_invalidate_range(base, base + PGSIZE);
}

#endif /* KRX_RISCV64_VMP_RISCV64_H */
