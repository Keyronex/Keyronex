/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vc_support.h
 * @brief Viewcache support functions
 */

 #include <sys/k_log.h>
 #include <sys/k_intr.h>

#include <vm/map.h>
#include <vm/page.h>
#include <sys/iop.h>
#include <sys/pmap.h>

#include <sys/proc.h>


void rs_evict_leaf_pte(struct vm_rs *rs, vaddr_t vaddr, vm_page_t *page,
    pte_t *pte);

void
vm_vc_unmap(vaddr_t addr, size_t size)
{
	vm_page_t *table_page;
	pte_t *ppte;
	size_t n_unmapped = 0;

	kassert(ke_ipl() == IPL_DISP);

	ke_spinlock_enter_nospl(&proc0.vm_map->creation_lock);
	ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);

	ppte = pmap_fetch_pte(proc0.vm_map, &table_page, addr);

	if (ppte == NULL)
		return;

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

	if (table_page->proctable.valid_pageable_leaf_ptes == 0) {
		kdprintf("vm_vc_unmap: inactive leaf page table page %p\n",
		    table_page);
		TAILQ_REMOVE(&proc0.vm_map->rs.active_leaf_tables, table_page,
		    qlink);
	}
	pmap_valid_ptes_zeroed(&proc0.vm_map->rs, table_page, n_unmapped);

	ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
	ke_spinlock_exit_nospl(&proc0.vm_map->creation_lock);
}

#define VIEWCACHE_VIEW_PAGES 16

void
vm_vc_clean(vm_object_t *vmobj, size_t offset, vaddr_t addr, size_t size)
{
	vm_page_t *pages[VIEWCACHE_VIEW_PAGES] = { 0 };
	bool dirty[VIEWCACHE_VIEW_PAGES] = { 0 };
	ipl_t ipl;

	ipl = spldisp();


	/* collect the pages, covered by the view, from the vm object */
	ke_spinlock_enter_nospl(&vmobj->stealing_lock);
	for (size_t i = 0; i < size >> PGSHIFT; i++) {
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

	ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);
	{
		pte_t *ppte = pmap_fetch_pte(proc0.vm_map, NULL, addr);
		if (ppte == NULL)
			goto out;

		for (size_t i = 0; i < size >> PGSHIFT; i++) {
			pte_t pte = pmap_load_pte(&ppte[i]);
			if (pmap_pte_characterise(pte) != kPTEKindHW)
				continue;

			if (pmap_pte_hwleaf_writeable(pte)) {
				kassert(pages[i] ==
				  pmap_pte_hwleaf_page(pte, PMAP_L0));

				/* make non-writeable */
				pmap_pte_hwleaf_clear_writeable(&ppte[i]);
				pmap_tlb_flush_vaddr_globally(addr +
				    (i << PGSHIFT));
				vm_page_dirty(pages[i]);
			}
		}
	}
out:
	ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);

	/*
	 * Clear the dirty status. Have to do before the I/O as they can be
	 * dirtied again.
	 */
	for (size_t i = 0; i < size >> PGSHIFT; i++) {
		if (pages[i] == NULL || !pages[i]->dirty)
			continue;

		vm_domain_t *dom = &vm_domains[pages[i]->domain];
		ke_spinlock_enter_nospl(&dom->queues_lock);
		if (pages[i]->dirty) {
			pages[i]->dirty = false;
			dirty[i] = true;
		}
		ke_spinlock_exit_nospl(&dom->queues_lock);
	}

	splx(ipl);

	for (size_t i = 0; i < VIEWCACHE_VIEW_PAGES;) {
		sg_seg_t sg_segs[VIEWCACHE_VIEW_PAGES];
		sg_list_t sgl;
		size_t run_start, run_len;
		iop_t *iop;

		if (pages[i] == NULL || !dirty[i]) {
			i++;
			continue;
		}

		/* page is resident and dirty, start a run */
		run_start = i;
		run_len = 0;

		while (i < VIEWCACHE_VIEW_PAGES && pages[i] != NULL) {
			sg_segs[run_len].paddr = VM_PAGE_PADDR(pages[i]);
			sg_segs[run_len].length = PGSIZE;
			run_len++;
			i++;

			/*
			 * next page isn't resident, or there are 2 clean pages
			 * in a row, stop the run.
			 */
			if (i < VIEWCACHE_VIEW_PAGES && pages[i] != NULL &&
			    !dirty[i]) {
				size_t clean_run = 0;
				for (size_t j = i; j < VIEWCACHE_VIEW_PAGES &&
				    pages[j] != NULL && !dirty[j];
				    j++)
					clean_run++;

				if (clean_run > 2)
					break;
			}
		}

		sgl.elems = sg_segs;
		sgl.elems_n = run_len;

		iop = iop_new_write(vmobj->vnode, &sgl, 0, run_len << PGSHIFT,
		    offset + (run_start << PGSHIFT));
		iop_send_sync(iop);
		iop_free(iop);

#if 0
		kprintf("vm_vc_clean: wrote %zu pages at offset 0x%zx\n",
		    run_len, offset + (run_start << PGSHIFT));
#endif
	}

	for (size_t i = 0; i < VIEWCACHE_VIEW_PAGES; i++) {
		if (pages[i] != NULL)
			vm_page_release(pages[i]);
	}
}
