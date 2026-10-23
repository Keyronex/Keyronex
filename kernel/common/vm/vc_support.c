/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vc_support.h
 * @brief Viewcache support functions
 */

#include <sys/iop.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/pmap.h>
#include <sys/proc.h>
#include <sys/vnode.h>

#include <vm/map.h>
#include <vm/page.h>

void rs_evict_leaf_pte(struct vm_rs *rs, vaddr_t vaddr, vm_page_t *page,
    pte_t *pte);

void
vm_vc_unmap(vaddr_t addr, size_t size)
{
	vm_page_t *table_page;
	pte_t *ppte;
	size_t n_unmapped = 0;
	vm_page_queue_t tables = TAILQ_HEAD_INITIALIZER(tables);

	kassert(ke_ipl() == IPL_DISP);

	ke_spinlock_enter_nospl(&proc0.vm_map->creation_lock);
	ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);

	ppte = pmap_fetch_pte(proc0.vm_map, &table_page, addr);

	if (ppte == NULL) {
		ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
		ke_spinlock_exit_nospl(&proc0.vm_map->creation_lock);
		return;
	}

	for (size_t i = 0; i < size / PGSIZE; i++) {
		pte_t pte = pmap_load_pte(&ppte[i]);

		if (pmap_pte_characterise(pte) != kPTEKindHW)
			continue;

		/* this function does a TLB shootdown each, so not most ideal */
		rs_evict_leaf_pte(&proc0.vm_map->rs, addr + (i << PGSHIFT),
		    pmap_pte_hwleaf_page(pte, 0), &ppte[i]);
		n_unmapped++;
	}

	/* x-ref valid_pageable_leaf_ptes */
	table_page->proctable.valid_pageable_leaf_ptes -= (n_unmapped);

	/* should be on the active list only if it had valid PTEs before */
	if (table_page->proctable.valid_pageable_leaf_ptes == 0 &&
	    n_unmapped > 0) {
		kdprintf("vm_vc_unmap: inactive leaf page table page %p\n",
		    table_page);
		VM_PAGE_QTAG_CLEAR(table_page, VM_PAGE_QTAG_LEAF);
		TAILQ_REMOVE(&proc0.vm_map->rs.active_leaf_tables, table_page,
		    qlink);
	}
	pmap_valid_ptes_zeroed(&proc0.vm_map->rs, table_page, n_unmapped,
	    &tables);

	ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
	ke_spinlock_exit_nospl(&proc0.vm_map->creation_lock);

	if (!TAILQ_EMPTY(&tables)) {
		ke_aspace_invalidate(&kernel_map.aspace, addr, size,
		    KE_INVAL_NONLEAF);
		pmap_free_tables(&kernel_map, &tables);
	}
}

#define VIEWCACHE_VIEW_PAGES 16

void
vm_vc_clean(vm_object_t *vmobj, size_t offset, vaddr_t addr, size_t size)
{
	vm_page_t *pages[VIEWCACHE_VIEW_PAGES] = { 0 };
	bool written[VIEWCACHE_VIEW_PAGES] = { 0 };
	bool dirty[VIEWCACHE_VIEW_PAGES] = { 0 };
	bool bridge[VIEWCACHE_VIEW_PAGES] = { 0 };
	size_t npages = size >> PGSHIFT;
	pte_t *ppte;
	ipl_t ipl;

	/*
	 * No replacement of the view (being in VIEW_WRITEBACK state) nor
	 * truncation of the vnode (or its teardown) is possible until we
	 * return.
	 */

	kassert(npages <= VIEWCACHE_VIEW_PAGES);

	vm_obj_io_enter(vmobj);

	ipl = spldisp();

	/* collect the pages, covered by the view, from the vm object */
	ke_spinlock_enter_nospl(&vmobj->stealing_lock);
	for (size_t i = 0; i < npages; i++) {
		pte_t *ppte = obj_fetch_pte(vmobj, offset + (i << PGSHIFT)),
			pte;

		if (ppte == NULL)
			continue;

		pte = pmap_load_pte(ppte);
		if (pmap_pte_characterise(pte) != kPTEKindHW)
			continue;

		pages[i] = pmap_pte_hwleaf_page(pte, 0);
		vm_page_retain(pages[i]);
	}
	ke_spinlock_exit_nospl(&vmobj->stealing_lock);

	/*
	 * Write-protect any writeable PTEs and note them as dirty. Future
	 * writes will fault and mark the pages dirty again.
	 * (TODO: consider invalidation batching?)
	 */
	ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);

	ppte = pmap_fetch_pte(proc0.vm_map, NULL, addr);
	if (ppte == NULL)
		goto collected;

	for (size_t i = 0; i < npages; i++) {
		pte_t pte = pmap_load_pte(&ppte[i]);
		if (pmap_pte_characterise(pte) != kPTEKindHW)
			continue;

		if (pmap_pte_hwleaf_writeable(pte)) {
			kassert(pages[i] == pmap_pte_hwleaf_page(pte, PMAP_L0));

			pmap_pte_hwleaf_clear_writeable(&ppte[i]);
			ke_aspace_invalidate(&kernel_map.aspace,
			    addr + (i << PGSHIFT), PGSIZE, KE_INVAL_LEAF);
			written[i] = true;
		}
	}

	ke_spinlock_enter_nospl(&vmobj->stealing_lock);
	for (size_t i = 0; i < npages; i++) {
		if (written[i])
			vm_obj_page_dirtied(vmobj, pages[i]);
	}
	ke_spinlock_exit_nospl(&vmobj->stealing_lock);

collected:
	ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
	splx(ipl);

	VOP_PAGING_ENTER(vmobj->vnobj.vnode);

	/*
	 * Claim the dirtiness of all the dirty pages covered by the view (and
	 * any pages dirtied by other means). Has to be done before I/O as the
	 * pages can be dirtied again meanwhile.
	 *
	 * We allow pages that weren't claimed to join runs for the sake of more
	 * contiguous I/O - but we must avoid any above the valid length in case
	 * we reextend a file being truncated.
	 */
	ipl = spldisp();
	ke_spinlock_enter_nospl(&vmobj->stealing_lock);
	for (size_t i = 0; i < npages; i++) {
		if (pages[i] == NULL)
			continue;

		dirty[i] = vm_obj_page_claim_dirty(vmobj, pages[i]);
		if (!dirty[i])
			bridge[i] = pages[i]->use == VM_PAGE_FILE &&
			    pages[i]->owner_obj == vmobj &&
			    offset + (i << PGSHIFT) < vmobj->vnobj.valid_length;
	}
	ke_spinlock_exit_nospl(&vmobj->stealing_lock);
	splx(ipl);

	for (size_t i = 0; i < npages;) {
		size_t run_start, run_end;

		if (pages[i] == NULL || !dirty[i]) {
			i++;
			continue;
		}

		run_start = run_end = i;

		for (;;) {
			size_t k = run_end + 1;

			while (k < npages && k - run_end <= 2 &&
			    pages[k] != NULL && !dirty[k] && bridge[k])
				k++;

			if (k < npages && pages[k] != NULL && dirty[k])
				run_end = k;
			else
				break;
		}

		vm_obj_write_pages(vmobj, &pages[run_start],
		    run_end - run_start + 1, offset + (run_start << PGSHIFT));

		i = run_end + 1;
	}

	VOP_PAGING_EXIT(vmobj->vnobj.vnode);

	for (size_t i = 0; i < npages; i++) {
		if (pages[i] != NULL)
			vm_page_release(pages[i]);
	}

	vm_obj_io_exit(vmobj);
}
