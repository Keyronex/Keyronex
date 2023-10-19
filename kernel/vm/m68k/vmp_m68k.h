#ifndef KRX_M68K_VMP_M68K_H
#define KRX_M68K_VMP_M68K_H

#include <stdint.h>

#include "kdk/vm.h"
#include "mmu.h"

struct vmp_pager_state;

enum software_pte_kind {
	/*! this is not a software PTE */
	kNotSoftware = 0x0,
	/*! this is a PTE for a page currently being paged in/out  */
	kTransition = 0x1,
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

typedef union pte {
	pte_hw_t hw;
	pte_sw_t sw;
} pte_t;

static inline void
vmp_md_pte_create_hw(pte_hw_t *pte, pfn_t pfn, bool writeable)
{
	pte->pfn = pfn;
	pte->cachemode = 1; /* cacheable, copyback */
	pte->supervisor = 1;
	pte->type = 3;
	pte->global = (pfn << 12) >= HIGHER_HALF;
	pte->writeprotect = writeable ? 0 : 1;
	pte->type = 1; /* resident */
}

static inline void
vmp_md_pte_create_busy(pte_t *pte, pfn_t pfn)
{
	pte->sw.type = 0;
	pte->sw.data = pfn;
	pte->sw.kind = kTransition;
}

static inline bool
vmp_md_pte_is_empty(pte_t *pte)
{
	return *(uint32_t*)pte == 0;
}

static inline bool
vmp_md_pte_is_valid(pte_t *pte)
{
	return (*(uint32_t*)pte & 0x3) != 0;
}

static inline bool
vmp_md_hw_pte_is_writeable(pte_hw_t *pte)
{
	return pte->writeprotect == 1;
}

#endif /* KRX_M68K_VMP_M68K_H */
