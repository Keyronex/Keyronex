#ifndef KRX_AMD64_VMP_AMD64_H
#define KRX_AMD64_VMP_AMD64_H

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
	enum software_pte_kind kind : 2;
	uint64_t data : 60, /* pfn, swap descriptor, fork pointer */
	    type : 2;		       /* must = 0*/
} pte_sw_t;

typedef struct __attribute__((packed)) pte_hw {
	union __attribute__((packed)) {
		uint64_t value;
		struct __attribute__((packed)) {
			bool valid : 1;
			bool writeable : 1;
			bool user : 1;
			bool writethrough : 1;
			bool nocache : 1;
			bool accessed : 1;
			bool dirty : 1;
			bool pat : 1;
			bool global : 1;
			uint64_t available1 : 3;
			uintptr_t pfn : 40;
			uint64_t available2 : 11;
			bool nx : 1;
		};
	};
} pte_hw_t;

typedef union pte {
	pte_hw_t hw;
	pte_sw_t sw;
} pte_t;

static inline void
vmp_md_pte_create_hw(pte_hw_t *pte, pfn_t pfn, bool writeable)
{
	int flags = kMMUPresent;
	if (writeable)
		flags |= kMMUWrite;

	amd64_pte_set((uint64_t*)pte, PFN_TO_PADDR(pfn), flags);
}

static inline void
vmp_md_pte_create_busy(pte_t *pte, pfn_t pfn)
{
	kfatal("Unimplemented\n");
}

static inline void
vmp_md_pte_create_zero(pte_t *pte)
{
	kfatal("Unimplemented\n");
}

static inline bool
vmp_md_pte_is_empty(pte_hw_t *pte)
{
	return pte->value == 0;
}

static inline bool
vmp_md_pte_is_valid(pte_hw_t *pte)
{
	return pte->value & kMMUPresent;
}

static inline bool
vmp_md_hw_pte_is_writeable(pte_hw_t *pte)
{
	return pte->value & kMMUWrite;
}


#endif /* KRX_AMD64_VMP_AMD64_H */
