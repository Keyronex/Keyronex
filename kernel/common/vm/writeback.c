/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/writeback.c
 * @brief Vnode VM object page writeback.
 *
 * Used by the viewcache, the flush driver here, and there will be a dirty page
 * writer at some point.
 *
 * For each page:
 * 1. We retain the page and pin the object for I/O.
 * 2. VOP_PAGING_ENTER() on the object's vnode.
 * 3. With obj->stealing_lock, the dirty flags are revalidated and cleared
 *    before the write (so that re-dirtying doesn't race),
 * 4. The write is issued with vm_obj_write_pages().
 * 5. VOP_PAGING_EXIT(), page is released, and the object is unpinned.
 *
 * VOP_PAGING_ENTER() allows us to revalidate against any new valid length in
 * step 3 so we avoid re-extending a file.
 */

#include <sys/iop.h>
#include <sys/k_log.h>
#include <sys/pmap.h>
#include <sys/vnode.h>

#include <vm/map.h>
#include <vm/page.h>

#define WRITEBACK_RUN_MAX 16

void
vm_obj_write_pages(vm_object_t *obj, vm_page_t **pages, size_t npages,
    uint64_t offset)
{
	sg_seg_t sg_segs[WRITEBACK_RUN_MAX];
	sg_list_t sgl;
	iop_t *iop;

	kassert(obj->kind == VM_OBJ_VNODE);
	kassert(npages > 0 && npages <= WRITEBACK_RUN_MAX);

	for (size_t i = 0; i < npages; i++) {
		sg_segs[i].paddr = VM_PAGE_PADDR(pages[i]);
		sg_segs[i].length = PGSIZE;
	}

	sgl.elems = sg_segs;
	sgl.elems_n = npages;

	/* TODO: Handle failures of this. */
	iop = iop_new_write(obj->vnobj.vnode, &sgl, 0, npages << PGSHIFT,
	    offset);
	iop_send_sync(iop);
	iop_free(iop);
}

/*
 * Collect dirtiness from any live MAP_SHARED mappings of the object. Any
 * writeable PTEs are write-protected and the page is recorded as dirty.
 *
 * TODO: both this and vm_obj_truncate_mappings() fetch every PTE individually!
 */
static void
collect_dirty_mappings(vm_object_t *obj)
{
	struct vm_map_entry *entry;

	ke_mutex_enter(&obj->map_entries_lock, "collect_dirty_mappings");

	LIST_FOREACH(entry, &obj->map_entries, object_link) {
		vm_map_t *map = entry->map;
		ipl_t ipl;

		if (entry->cow)
			continue;

		ipl = spldisp();
		ke_spinlock_enter_nospl(&map->creation_lock);
		ke_spinlock_enter_nospl(&map->stealing_lock);

		for (vaddr_t va = entry->start; va < entry->end; va += PGSIZE) {
			pte_t *ppte, pte;
			vm_page_t *page;

			ppte = pmap_fetch_pte(map, NULL, va);
			if (ppte == NULL)
				continue;

			pte = pmap_load_pte(ppte);
			if (pmap_pte_characterise(pte) != kPTEKindHW ||
			    !pmap_pte_hwleaf_writeable(pte))
				continue;

			page = pmap_pte_hwleaf_page(pte, PMAP_L0);
			if (page->use != VM_PAGE_FILE)
				continue;

			kassert(page->owner_obj == obj);

			pmap_pte_hwleaf_clear_writeable(ppte);
			ke_aspace_invalidate(&map->aspace, va, PGSIZE,
			    KE_INVAL_LEAF);

			ke_spinlock_enter_nospl(&obj->stealing_lock);
			vm_obj_page_dirtied(obj, page);
			ke_spinlock_exit_nospl(&obj->stealing_lock);
		}

		ke_spinlock_exit_nospl(&map->stealing_lock);
		ke_spinlock_exit_nospl(&map->creation_lock);
		splx(ipl);
	}

	ke_mutex_exit(&obj->map_entries_lock);
}

/*
 * Write back every dirty page of the object in contiguous runs.
 * vnode paging lock must be held.
 * TODO: could any logic be factored between this and vm_vc_clean()?
 */
static void
flush_pages(vm_object_t *obj)
{
	uint64_t offset = 0;

	while (offset < obj_max_offset) {
		vm_page_t *pages[WRITEBACK_RUN_MAX];
		uint64_t run_offset = 0;
		size_t n = 0, inspected = 0;
		ipl_t ipl;

		ipl = spldisp();
		ke_spinlock_enter_nospl(&obj->stealing_lock);

		if (offset >= obj->vnobj.valid_length) {
			/* nothing to write beyond this point */
			ke_spinlock_exit_nospl(&obj->stealing_lock);
			splx(ipl);
			break;
		}

		/*
		 * We look at most at a table's worth of OTEs before releasing
		 * the lock to avoid holding it too long.
		 */
		while (n < WRITEBACK_RUN_MAX && offset < obj_max_offset &&
		    inspected++ < PGSIZE / sizeof(pte_t)) {
			uint64_t next;
			pte_t *ppte, pte;
			vm_page_t *page;

			/*
			 * Checked before the fetch, not only per dirty page,
			 * so the walk never goes past the valid length (which
			 * also keeps offset within size_t on 32-bit targets).
			 */
			if (offset >= obj->vnobj.valid_length)
				break;

			ppte = obj_fetch_pte_skip(obj, offset, &next);
			if (ppte == NULL) {
				offset = next;
				if (n > 0)
					break;
				continue;
			}

			pte = pmap_load_pte(ppte);
			if (pmap_pte_characterise(pte) != kPTEKindHW) {
				/* zero, or busy (a page-in; not dirty) */
				offset = next;
				if (n > 0)
					break;
				continue;
			}

			page = pmap_pte_hwleaf_page(pte, PMAP_L0);
			kassert(page->use == VM_PAGE_FILE);
			kassert(page->owner_obj == obj);

			if (!page->shared.dirty) {
				offset = next;
				if (n > 0)
					break;
				continue;
			}

			/*
			 * Page is dirty. Retain for our write (also takes it
			 * off any paging queues) and claim the dirty flag.
			 */
			vm_page_retain(page);
			if (!vm_obj_page_claim_dirty(obj, page)) {
				vm_page_release(page);
				offset = next;
				if (n > 0)
					break;
				continue;
			}

			if (n == 0)
				run_offset = offset;
			pages[n++] = page;
			offset = next;
		}

		ke_spinlock_exit_nospl(&obj->stealing_lock);
		splx(ipl);

		if (n == 0)
			continue;

		vm_obj_write_pages(obj, pages, n, run_offset);

		for (size_t i = 0; i < n; i++)
			vm_page_release(pages[i]);
	}
}

void
vm_obj_flush(vm_object_t *obj)
{
	kassert(obj->kind == VM_OBJ_VNODE);

	collect_dirty_mappings(obj);

	/*
	 * Anyone else that tries to flush will wait for our writes to finish.
	 * We can pin since the vnode is held.
	 */
	vm_obj_io_enter(obj);
	VOP_PAGING_ENTER(obj->vnobj.vnode);
	flush_pages(obj);
	VOP_PAGING_EXIT(obj->vnobj.vnode);
	vm_obj_io_exit(obj);

	vm_obj_drain_io(obj);
}
