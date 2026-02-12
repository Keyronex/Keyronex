/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/pmap.c
 * @brief Physical mapping code.
 */

#include <keyronex/dlog.h>
#include <keyronex/pmap.h>

#include <libkern/lib.h>

#include "vm/map.h"
#include "vm/page.h"

/* todo: rationalise this */
void pmap_valid_ptes_zeroed(struct vm_rs *rs, vm_page_t *page, size_t n);

int
pmap_wire_pte(vm_map_t *map, struct vm_rs *rs, struct pte_cursor *state,
    vaddr_t vaddr, bool create)
{
	size_t indexes[PMAP_MAX_LEVELS];
	pte_t *table;
	vm_page_t *table_page;

	kassert(ke_spinlock_held(&map->creation_lock), "");
	kassert(ke_spinlock_held(&map->stealing_lock), "");
	kassert(ke_ipl() == IPL_DISP, "");

	memset(state, 0, sizeof(*state));
	pmap_indexes(vaddr, indexes);

	table = (pte_t *)p2v(map->pgtable);
	table_page = VM_PAGE_FOR_PADDR(map->pgtable);

	kassert(table_page->use == VM_PAGE_TABLE, "");
	kassert(table_page->proctable.level == PMAP_LEVELS - 1, "");

	state->pages[PMAP_LEVELS - 1] = table_page;

	for (pmap_level_t level = PMAP_LEVELS - 1;; level--) {
		pte_t *ppte = &table[indexes[level]], pte;
		vm_page_t *next_page;

		if (level == 0) {
			state->pte = ppte;
			return 0;
		}

		pte = pmap_load_pte(ppte);

		switch (pmap_pte_characterise(pte)) {
		case kPTEKindZero: {
			if (!create)
				return -level;

#if DEBUG_WIRE_PTE
			kprintf("At Level %d: ZERO, "
				"refcnt %d, nonzero PTEs %d\n",
			    level, table_page->refcnt,
			    table_page->nonzero_ptes);
#endif

			ke_spinlock_exit_nospl(&map->stealing_lock);

			next_page = vm_page_alloc(VM_PAGE_TABLE, 0, VM_DOMID_LOCAL,
			    0);
			if (next_page == NULL)
				kfatal("TODO: Wait on pages avail event.\n");

			memset((void *)vm_page_hhdm_addr(next_page), 0, PGSIZE);

			ke_spinlock_enter_nospl(&map->stealing_lock);

			next_page->pte = ppte;
			next_page->owner_rs = rs;
			next_page->proctable.level = level - 1;
			if (level - 1 == 0) {
				/* fixme: not right for cases where there's
				 * multiple PTEs created for same table (m68k).
				 * 12/02/26: actually it *is* right. our groups
				 * of L0 tables in m68k always cover
				 * PGSIZE/sizeof(pte_t) * PGSIZE bytes. so it
				 * works. we will need to revisit this if we add
				 * support for leaf PTEs in levels > 0 though.
				 */
				size_t entries = PGSIZE / sizeof(pte_t);
				vaddr_t mask = ~((vaddr_t)(entries * PGSIZE) -
				    1);
				next_page->proctable.base = (vaddr & mask) >>
				    PGSHIFT;
			}
			next_page->proctable.is_root = false;
			/* refcnt is already 1; make these 1 to pin. */
			next_page->proctable.nonzero_ptes = 1;
			next_page->proctable.noswap_ptes = 1;
			next_page->proctable.valid_pageable_leaf_ptes = 0;

			table_page->proctable.nonzero_ptes++;
			table_page->proctable.noswap_ptes++;

			pte = pmap_pte_hwdir_create(ppte, VM_PAGE_PADDR(next_page),
			    level);

			state->pages[level - 1] = next_page;

			table = (pte_t *)p2v(pmap_pte_hwdir_paddr(pte, level));
			table_page = next_page;

			break;
		}

		case kPTEKindHW: {
			next_page = VM_PAGE_FOR_PADDR(pmap_pte_hwdir_paddr(pte, level));

#if DEBUG_WIRE_PTE
			kprintf("At Level %d: HW, "
				"refcnt %d, nonzero PTEs %d\n",
			    level, table_page->refcnt,
			    table_page->nonzero_ptes);
#endif

			/* pin the next level */

			next_page->proctable.noswap_ptes++;
			next_page->proctable.nonzero_ptes++;

			state->pages[level - 1] = next_page;

			table = (pte_t *)p2v(pmap_pte_hwdir_paddr(pte, level));
			table_page = next_page;

			/* sanity checks */
			kassert(table_page->use == VM_PAGE_TABLE, "");
			kassert(table_page->proctable.level == level - 1, "");

			break;
		}

#if 0
		case kPTEKindTrans: {
			vm_page_t *page = pmap_pte_soft_page(pte);

			vm_page_retain(page);

			pmap_pte_hw_create_table(pte, VM_PAGE_PADDR(page),
			    level);

			/* pin the next level */

			page->proctable.noswap_ptes++;
			page->proctable.nonzero_ptes++;

			state->pages[level - 1] = page;
			table = (pte_t *)vm_page_hhdm_addr(page);
			table_page = page;

			/* sanity checks */
			kassert(table_page->use == VM_PAGE_TABLE);
			kassert(table_page->proctable.level == level - 1);

			break;
		}
#endif

		case kPTEKindSwap: {
			kfatal("Swap directory PTE - Implement me\n");
		}

		case kPTEKindBusy: {
			kfatal("Busy directory PTE - Implement me\n");
		}

		default:
			kfatal("Implement me\n");
		}
	}

	return 0;
}

void
pmap_unwire_pte(struct vm_map *map, struct vm_rs *rs,
    struct pte_cursor *state)
{
	kassert(ke_spinlock_held(&map->creation_lock), "");
	kassert(ke_spinlock_held(&map->stealing_lock), "");

	for (size_t i = 0; i < PMAP_LEVELS - 1; i++) {
		vm_page_t *page = state->pages[i];
		kassert(page->use == VM_PAGE_TABLE, "");
		kassert(page->proctable.level == i, "");
		pmap_valid_ptes_zeroed(rs, page, 1);
	}
}


void
pmap_valid_ptes_zeroed(vm_rs_t *rs, vm_page_t *page, size_t n)
{
	kassert(page->use == VM_PAGE_TABLE, "");

	page->proctable.nonzero_ptes -= n;

	if (page->proctable.nonzero_ptes == 0) {
		vm_page_t *dir_page;
		kassert(!page->proctable.is_root, "freeing root pgtable");
		dir_page = VM_PAGE_FOR_HHDM_ADDR((vaddr_t)page->pte);
		pmap_pte_zerodir_create(page->pte, page->proctable.level + 1);
		vm_page_delete(page, true);
		pmap_valid_ptes_zeroed(rs, dir_page, 1);
	} else {
		page->proctable.noswap_ptes -= n;
		if (page->proctable.noswap_ptes == 0 &&
		    !page->proctable.is_root) {
#if 0
			pmap_pte_hwdir_create_soft(page->pte, kPTEKindTrans,
			    VM_PAGE_PFN(page), true);
			/* todo(low): page table dirtiness tracking? */
			vm_page_release_and_dirty(page, true);
#else
			kfatal("Implement me: unpinning pagetable\n");
#endif
		}
	}
}
