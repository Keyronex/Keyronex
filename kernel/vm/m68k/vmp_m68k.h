#ifndef KRX_M68K_VMP_M68K_H
#define KRX_M68K_VMP_M68K_H

#include <stdint.h>

#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "mmu_040.h"

#define VMP_TABLE_LEVELS 3
#define VMP_PAGE_SHIFT 12

/*
 * there's actually only 128 entries at l2 and 64 at l1;
 * but we allocate a whole page worth of PML2s and PML1s and use the step factor
 * to skip entries which point to the same logical page.
 */
#define VMP_LEVEL_3_ENTRIES 128
#define VMP_LEVEL_3_STEP 16
#define VMP_LEVEL_2_ENTRIES 1024
#define VMP_LEVEL_2_STEP 16
#define VMP_LEVEL_1_ENTRIES 1024

struct vmp_pager_state;

enum software_pte_kind {
	/*! PTE represents an address in swap. */
	kSoftPteKindSwap,
	/*! PTE represents a page being read-in from disk. */
	kSoftPteKindBusy,
	/*! PTE is transitional between memory and disk; not in working set. */
	kSoftPteKindTrans,
};

struct vmp_md_procstate {
	/*! physical address of the translation table */
	paddr_t table;
};

typedef struct __attribute__((packed)) pte_sw {
	enum software_pte_kind kind : 2;
	uint32_t data : 28; /* pfn, swap descriptor, fork pointer */
	uint8_t type : 2; /* matches hardware; must = 0*/
} pte_sw_t;

typedef union __attribute__((packed)) pte {
	pml3e_040_t hw_pml3_040;
	pml2e_040_t hw_pml2_040;
	pml1e_040_t hw_pml1_040;
	pte_sw_t sw;
	uint32_t value;
} pte_t;

static inline bool
vmp_md_pte_is_empty(pte_t *pte)
{
	return *(uint32_t*)pte == 0;
}

static inline bool
vmp_md_pte_is_valid(pte_t *pte)
{
	return (pte->value & 0x3) != 0;
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
	else {
		kassert(pte->sw.kind == kSoftPteKindSwap);
		return kPTEKindSwap;
	}
}

static inline void
vmp_md_pte_create_hw(pte_t *pte, pfn_t pfn, bool writeable, bool cacheable)
{
	pte_t newpte;
	newpte.hw_pml1_040.pfn = pfn;
	/* 1 = cached/copyback; 3 = uncached. (2 = uncached/serialised) */
	newpte.hw_pml1_040.cachemode = cacheable ? 1 : 3;
	newpte.hw_pml1_040.supervisor = 1;
	newpte.hw_pml1_040.type = 3;
	newpte.hw_pml1_040.global = (pfn << 12) >= HIGHER_HALF;
	newpte.hw_pml1_040.writeprotect = writeable ? 0 : 1;
	newpte.hw_pml1_040.type = 1; /* resident */
	pte->value = newpte.value;
}

static inline void
vmp_md_pte_create_busy(pte_t *pte, pfn_t pfn)
{
	pte->sw.type = 0;
	pte->sw.data = pfn;
	pte->sw.kind = kSoftPteKindBusy;
}

static inline void
vmp_md_pte_create_trans(pte_t *pte, pfn_t pfn)
{
	pte->sw.type = 0;
	pte->sw.data = pfn;
	pte->sw.kind = kSoftPteKindTrans;
}

static inline void
vmp_md_pte_create_zero(pte_t *pte)
{
	pte->value = 0;
}

static inline pfn_t
vmp_md_pte_hw_pfn(pte_t *pte, int level)
{
	switch (level) {
	case 1:
		return pte->hw_pml1_040.pfn;
	case 2:
		return pte->hw_pml2_040.addr << 8;
	case 3:
		return pte->hw_pml3_040.addr << 8;
	default:
		kfatal("unexpected level");
	}
}

static inline paddr_t
vmp_pte_hw_paddr(pte_t *pte, int level)
{
	switch (level) {
	case 1:
		return pte->hw_pml1_040.pfn << 12;
	case 2:
		return pte->hw_pml2_040.addr << 4;
	case 3:
		return pte->hw_pml3_040.addr << 4;
	default:
		kfatal("unexpected level");
	}
}

static inline bool
vmp_md_hw_pte_is_writeable(pte_t *pte)
{
	return pte->hw_pml1_040.writeprotect == 0;
}

static inline pfn_t
vmp_md_soft_pte_pfn(pte_t *pte)
{
	kassert(vmp_pte_characterise(pte) == kPTEKindBusy || vmp_pte_characterise(pte) == kPTEKindTrans);
	return pte->sw.data;
}

static inline void
vmp_addr_unpack(vaddr_t vaddr, int unpacked[VMP_TABLE_LEVELS + 1])
{
	union vaddr_040 addr;
	addr.addr = vaddr;
	unpacked[0] = addr.pgi;
	unpacked[1] = addr.l1i;
	unpacked[2] = addr.l2i;
	unpacked[3] = addr.l3i;
}

#endif /* KRX_M68K_VMP_M68K_H */
