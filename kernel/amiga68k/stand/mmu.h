#ifndef AMIGA68K_STAND_MMU_H_
#define AMIGA68K_STAND_MMU_H_

#include <stdint.h>

/*
 * we assume using only 4kib pages.
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

/* root table descriptor */
typedef struct pml3e {
	uint32_t addr : 28, used : 1, writeable : 1, type : 2;
} pml3e_t;

/* pointer table descriptor */
typedef struct pml2e {
	uint32_t addr : 28, used : 1, writeable : 1, type : 2;
} pml2e_t;

/* 4k page-level page descriptor */
typedef struct pml1e {
	uint32_t pfn : 20, userreserved : 1, global : 1, user1 : 1, user0 : 1,
	    supervisor : 1, cachemode : 2, modified : 1, used : 1,
	    writeable : 1, type : 2;
} pml1e_t;

_Static_assert(sizeof(pml3e_t) == sizeof(uint32_t), "Bad pml3e size");
_Static_assert(sizeof(pml2e_t) == sizeof(uint32_t), "Bad pml2e size");
_Static_assert(sizeof(pml1e_t) == sizeof(uint32_t), "Bad pml1e size");

typedef pml3e_t pml3_t[128];
typedef pml2e_t pml2_t[128];
typedef pml1e_t pml1_t[64];

#endif /* AMIGA68K_STAND_MMU_H_ */
