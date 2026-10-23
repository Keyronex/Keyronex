/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file unmap.c
 * @brief Batch release for unmapped pages and emptied page tables.
 */

#include <sys/k_log.h>
#include <sys/kmem.h>

#include <vm/map.h>
#include <vm/page.h>

#define ENTRY_DIRTY 0x1ul

void
vm_unmap_batch_init(struct vm_unmap_batch *batch, vm_map_t *map)
{
	batch->map = map;
	batch->count = 0;
	batch->lo = ~(vaddr_t)0;
	batch->hi = 0;
	TAILQ_INIT(&batch->tables);
}

void
vm_unmap_batch_extend(struct vm_unmap_batch *batch, vaddr_t va)
{
	if (va < batch->lo)
		batch->lo = va;
	if (va + PGSIZE > batch->hi)
		batch->hi = va + PGSIZE;
}

void
vm_unmap_batch_add(struct vm_unmap_batch *batch, vaddr_t va, vm_page_t *page,
    bool dirty)
{
	if (batch->count == VM_UNMAP_BATCH_SIZE)
		vm_unmap_batch_flush(batch);

	vm_unmap_batch_extend(batch, va);
	batch->entries[batch->count++] = (uintptr_t)page |
	    (dirty ? ENTRY_DIRTY : 0);
}

static void
release_page(vm_page_t *page, bool dirty)
{
	switch (page->use) {
	case VM_PAGE_PRIVATE:
	case VM_PAGE_KWIRED:
		vm_page_delete(page, true);
		break;

	case VM_PAGE_FILE:
	case VM_PAGE_ANON_SHARED: {
		vm_object_t *obj = page->owner_obj;

		ke_spinlock_enter_nospl(&obj->stealing_lock);
		if (dirty)
			vm_obj_page_dirtied(obj, page);
		if (--page->shared.share_count == 0)
			vm_page_release_and_dirty(page, page->shared.dirty);
		ke_spinlock_exit_nospl(&obj->stealing_lock);
		break;
	}

	case VM_PAGE_ANON_FORKED: {
		struct vm_anon *anon = page->owner_anon;
		bool last;

		/* TODO: dirtiness? */
		/*
		 * TODO: dedup the anon release logic?
		 * see unmap_ptes()'s fork PTE case;
		 * do_dirty_fork_fault()
		 */
		ke_spinlock_enter_nospl(&anon_creation_lock);
		last = --page->shared.share_count == 0;
		if (--anon->refcount == 0) {
			kassert(last);
			ke_spinlock_exit_nospl(&anon_creation_lock);
			vm_page_delete(page, true);
			kmem_free(anon, sizeof(struct vm_anon));
		} else {
			if (last)
				vm_page_release(page);
			ke_spinlock_exit_nospl(&anon_creation_lock);
		}
		break;
	}

	default:
		kfatal("vm_unmap_batch: unexpected page use %d\n", page->use);
	}
}

void
vm_unmap_batch_flush(struct vm_unmap_batch *batch)
{
	bool tables = !TAILQ_EMPTY(&batch->tables);

	if (batch->count == 0 && batch->hi == 0 && !tables)
		return;

	if (batch->hi != 0)
		ke_aspace_invalidate(&batch->map->aspace, batch->lo,
		    batch->hi - batch->lo,
		    tables ? KE_INVAL_NONLEAF : KE_INVAL_LEAF);
	else
		ke_aspace_invalidate_all(&batch->map->aspace, KE_INVAL_NONLEAF);
	batch->lo = ~(vaddr_t)0;
	batch->hi = 0;

	for (size_t i = 0; i < batch->count; i++) {
		uintptr_t e = batch->entries[i];

		release_page((vm_page_t *)(e & ~ENTRY_DIRTY),
		    (e & ENTRY_DIRTY) != 0);
	}
	batch->count = 0;

	pmap_free_tables(batch->map, &batch->tables);
}
