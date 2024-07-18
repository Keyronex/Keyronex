
#ifndef KRX_AARCH64_VMP_AARCH64_H
#define KRX_AARCH64_VMP_AARCH64_H

#include <stdint.h>

#include "kdk/vm.h"
#include "mmu_regs.h"

#define VMP_TABLE_LEVELS 4
#define VMP_PAGE_SHIFT 12

#define VMP_LEVEL_4_ENTRIES 512
#define VMP_LEVEL_3_ENTRIES 512
#define VMP_LEVEL_2_ENTRIES 512
#define VMP_LEVEL_1_ENTRIES 512

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
	uint64_t valid : 1, /* must = 0*/
	    data : 61;	    /* pfn, swap descriptor, fork pointer */
	enum software_pte_kind kind : 2;
} pte_sw_t;

typedef union __attribute__((packed)) pte {
	pte_hw_t hw;
	struct table_entry hw_table;
	pte_sw_t sw;
	uintptr_t value;
} pte_t;

static inline void
vmp_md_pte_create_hw(pte_t *ppte, pfn_t pfn, bool writeable, bool cacheable)
{
	pte_t pte;

	pte.value = 0x0;
	pte.hw.pfn = pfn;
	pte.hw.valid = 1;
	pte.hw.af = 1;
	pte.hw.ap = writeable ? 0b01 : 0b10;
	pte.hw.sh = 0b11;
	pte.hw.attrindx = cacheable ? 0 : 1;
	pte.hw.reserved_must_be_1 = 1;

	ppte->value = pte.value;
}

static inline void
vmp_md_pte_create_busy(pte_t *ppte, pfn_t pfn)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = pfn;
	pte.sw.kind = kSoftPteKindBusy;
	ppte->value = pte.value;
}

static inline void
vmp_md_pte_create_trans(pte_t *ppte, pfn_t pfn)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = pfn;
	pte.sw.kind = kSoftPteKindTrans;
	ppte->value = pte.value;
}

static inline void
vmp_md_pte_create_swap(pte_t *ppte, pfn_t pfn)
{
	pte_t pte;
	pte.sw.valid = 0;
	pte.sw.data = pfn;
	pte.sw.kind = kSoftPteKindSwap;
	ppte->value = pte.value;
}

static inline void
vmp_md_pte_create_zero(pte_t *pte)
{
	pte->value = 0;
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
vmp_md_hw_pte_is_writeable(pte_t *pte)
{
	return pte->hw.ap == 0b00 || pte->hw.ap == 0b01;
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

static inline pfn_t
vmp_md_soft_pte_pfn(pte_t *pte)
{
	kassert(vmp_pte_characterise(pte) == kPTEKindBusy ||
	    vmp_pte_characterise(pte) == kPTEKindTrans ||
	    vmp_pte_characterise(pte) == kPTEKindSwap);
	return pte->sw.data;
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

#endif /* KRX_AARCH64_VMP_AARCH64_H */
