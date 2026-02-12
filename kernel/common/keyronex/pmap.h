/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file pmap.h
 * @brief Physical mapping definitions.
 *
 * (this header probably should be privatised?)
 */

#ifndef ECX_KEYRONEX_PMAP_H
#define ECX_KEYRONEX_PMAP_H

#include <keyronex/pmap_arch.h>
#include <vm/page.h>

struct vm_map;

/* PTE cursor. Wires the page table pages. */
struct pte_cursor {
	vm_page_t *pages[PMAP_MAX_LEVELS];
	pte_t *pte;
};

enum pmap_pte_kind {
	kPTEKindZero = 0,
	kPTEKindSwap,
	kPTEKindTrans,
	kPTEKindBusy,
	kPTEKindFork,
	kPTEKindHW,
};

static inline enum pmap_pte_kind
pmap_pte_characterise(pte_t pte)
{
	if (pmap_pte_is_hw(pte))
		return kPTEKindHW;
	else
		return pte.soft.kind;
}

int pmap_wire_pte(struct vm_map *map, struct vm_rs *rs,
    struct pte_cursor *state, vaddr_t vaddr, bool create);
void pmap_unwire_pte(struct vm_map *map, struct vm_rs *rs,
    struct pte_cursor *state);

#endif /* ECX_KEYRONEX_PMAP_H */
