#ifndef KRX_AARCH64_VMP_AARCH64_H
#define KRX_AARCH64_VMP_AARCH64_H

#include <stdint.h>

#include "kdk/vm.h"
#include "mmu_regs.h"

enum software_pte_kind {
	/*! this is not a software PTE */
	kNotSoftware = 0x0,
	/*! this is a PTE for a */
	kTransition = 0x1,
};

struct vmp_md_procstate {
	/*! physical address of the translation table */
	paddr_t table;
};

typedef struct __attribute__((packed)) pte_sw {
	uint64_t valid: 1, /* must = 0 */
		data: 61;
	enum software_pte_kind kind : 2;
} pte_sw_t;

typedef union pte {
	pte_hw_t hw;
	pte_sw_t sw;
} pte_t;

static inline void
vmp_md_pte_create_hw(pte_hw_t *pte, pfn_t pfn, bool writeable)
{
	pte->page = pfn;
	pte->valid = 1;
	pte->reserved_must_be_1 = 1;
	pte->sh = 3;
	pte->af = 1;
}

static inline bool
vmp_md_pte_is_empty(pte_hw_t *pte)
{
	return *(uint32_t*)pte == 0;
}

static inline bool
vmp_md_pte_is_valid(pte_hw_t *pte)
{
	return (*(uint32_t*)pte & 0x3) != 0;
}

static inline bool
vmp_md_hw_pte_is_writeable(pte_hw_t *pte)
{
	return (pte->ap & 0b10) == 0;
}

#endif /* KRX_AARCH64_VMP_AARCH64_H */
