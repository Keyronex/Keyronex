/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/init.c
 * @brief Virtual memory maps.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/kmem.h>

#include <stdatomic.h>
#include <vm/map.h>

static int map_entry_cmp(struct vm_map_entry *x, struct vm_map_entry *y);

RB_GENERATE(vm_map_tree, vm_map_entry, rb_link, map_entry_cmp);
vm_map_t kernel_map;


static int
map_entry_cmp(struct vm_map_entry *x, struct vm_map_entry *y)
{
	/*
	 * what this actually does is determine whether x's start address is
	 * lower than, greater than, or within the bounds of Y. it works because
	 * we allocate virtual address space with vmem, which already ensures
	 * there are no overlaps.
	 */

	if (x->start < y->start)
		return -1;
	else if (x->start >= y->end)
		return 1;
	else
		/* x->start is within VAD y */
		return 0;
}

struct vm_map_entry *
vm_map_lookup(vm_map_t *map, vaddr_t addr)
{
	struct vm_map_entry key = {
		.start = addr,
	};
	return RB_FIND(vm_map_tree, &map->entries, &key);
}

vm_map_t *
vm_map_create(void)
{
	vm_map_t *map;

	map = kmem_alloc(sizeof(vm_map_t));
	if (map == NULL)
		return NULL;

	atomic_store_explicit(&map->refcnt, 1, memory_order_relaxed);

	RB_INIT(&map->entries);
	ke_rwlock_init(&map->map_lock);

	ke_spinlock_init(&map->creation_lock);
	ke_spinlock_init(&map->stealing_lock);

	map->rs.map = map;
	map->rs.private_pages_n = 0;
	map->rs.valid_n = 0;
	TAILQ_INIT(&map->rs.active_leaf_tables);

	map->pgtable = pmap_allocate_pgtable(map);

	vmem_init(&map->vmem, "userland", LOWER_HALF, LOWER_HALF_SIZE, PGSIZE,
	    NULL, NULL, NULL, 0, 0);

	return map;
}

void
vm_map_release(vm_map_t *map)
{
	if (atomic_fetch_sub_explicit(&map->refcnt, 1, memory_order_acq_rel) ==
	    1) {
#if TRACE_TODO
		kprintf_dbg("= TODO: vm_map_release freeing map %p\n", map);
#endif
	}
}

/*
 * mapping
 */

int
vm_allocate(vm_map_t *map, vm_prot_t prot, vaddr_t *vaddrp, size_t size,
    bool exact)
{
	return vm_map(map, NULL, vaddrp, size, 0, prot, prot, false, false,
	    exact);
}

int
vm_map(vm_map_t *map, vm_object_t *object, vaddr_t *vaddrp, size_t size,
    uint64_t obj_offset, vm_prot_t initial_prot, vm_prot_t max_prot,
    bool inherit_shared, bool cow, bool exact)
{
	int r;
	struct vm_map_entry *map_entry;
	vmem_addr_t addr = exact ? *vaddrp : 0;

	kassert(size % PGSIZE == 0);
	kassert(obj_offset % PGSIZE == 0);

	ke_rwlock_enter_write(&map->map_lock, "vm_map:map_lock");

	r = vmem_xalloc(&map->vmem, size, 0, 0, 0, addr, 0,
	    exact ? VM_EXACT : 0, &addr);
	if (r < 0) {
		ke_rwlock_exit_write(&map->map_lock);
		return r;
	}

	map_entry = kmem_alloc(sizeof(struct vm_map_entry));
	map_entry->start = addr;
	map_entry->end = addr + size;
	map_entry->max_prot = max_prot;
	map_entry->prot = initial_prot;
	map_entry->inherit_shared = inherit_shared;
	map_entry->cow = cow;
	map_entry->object = object;
	map_entry->offset = obj_offset;
	map_entry->is_phys = false;

#if TRACE_VM_MAP
	kprintf_dbg(" - made %p: %p-%p, obj=%p cow=%d shared=%d\n", map_entry, map_entry->start,
	    map_entry->end, object, cow, inherit_shared);
#endif

	RB_INSERT(vm_map_tree, &map->entries, map_entry);

	ke_rwlock_exit_write(&map->map_lock);

	*vaddrp = addr;

	return 0;
}

int
vm_map_phys(vm_map_t *map, paddr_t paddr, vaddr_t *vaddrp, size_t size,
    vm_prot_t prot, vm_cache_mode_t cache, bool exact)
{
	int r;
	struct vm_map_entry *map_entry;
	struct pte_cursor cursor;
	vmem_addr_t addr = exact ? *vaddrp : 0;
	ipl_t ipl;
	bool user;

	kassert(size % PGSIZE == 0);
	kassert(paddr % PGSIZE == 0);

	ke_rwlock_enter_write(&map->map_lock, "vm_map_phys:map_lock");

	r = vmem_xalloc(&map->vmem, size, 0, 0, 0, addr, 0,
	    exact ? VM_EXACT : 0, &addr);
	if (r < 0) {
		ke_rwlock_exit_write(&map->map_lock);
		return r;
	}

	if (addr < HIGHER_HALF)
		prot |= VM_USER;

	map_entry = kmem_alloc(sizeof(struct vm_map_entry));
	map_entry->start = addr;
	map_entry->end = addr + size;
	map_entry->max_prot = prot;
	map_entry->prot = prot;
	map_entry->inherit_shared = true; /* physical mappings are shared */
	map_entry->cow = false;
	map_entry->is_phys = true;
	map_entry->cache = cache;
	map_entry->phys_base = paddr;
	map_entry->offset = 0;

	RB_INSERT(vm_map_tree, &map->entries, map_entry);

	ipl = spldisp();
	ke_spinlock_enter_nospl(&map->creation_lock);
	ke_spinlock_enter_nospl(&map->stealing_lock);

	for (vaddr_t vaddr = addr; vaddr < addr + size; vaddr += PGSIZE) {
		paddr_t page_paddr = paddr + (vaddr - addr);

		r = pmap_wire_pte(map, &map->rs, &cursor, vaddr, true);
		if (r != 0)
			kfatal("vm_map_phys: failed to wire PTE\n");

		pmap_pte_hwleaf_create(cursor.pte, page_paddr >> PGSHIFT,
		    PMAP_L0, prot, cache);

		map->rs.valid_n++;
		cursor.pages[0]->proctable.nonzero_ptes++;
		cursor.pages[0]->proctable.noswap_ptes++;

		pmap_unwire_pte(map, &map->rs, &cursor);
	}

	ke_spinlock_exit_nospl(&map->stealing_lock);
	ke_spinlock_exit_nospl(&map->creation_lock);
	splx(ipl);

	ke_rwlock_exit_write(&map->map_lock);

	*vaddrp = addr;

	return 0;
}

/*
 * unmapping
 */

#define UNMAP_BATCH_SIZE 64

struct unmap_deferred {
	enum {
		DEFERRED_PRIVATE,
		DEFERRED_SHARED,
		DEFERRED_FORKED,
	} action;
	vm_page_t *page;
	union {
		struct vm_anon *anon;
		vm_object_t *obj;
	};
	bool dirty;
};

struct unmap_batch {
	struct unmap_deferred entries[UNMAP_BATCH_SIZE];
	size_t count;
};

static void
unmap_batch_init(struct unmap_batch *batch)
{
	batch->count = 0;
}

static void
unmap_batch_flush(struct unmap_batch *batch)
{
	if (batch->count == 0)
		return;

	pmap_tlb_flush_all_globally();

	for (size_t i = 0; i < batch->count; i++) {
		struct unmap_deferred *d = &batch->entries[i];

		switch (d->action) {
		case DEFERRED_PRIVATE:
			vm_page_delete(d->page, true);
			break;

		case DEFERRED_SHARED:
			ke_spinlock_enter_nospl(&d->obj->stealing_lock);
			d->page->shared.dirty |= d->dirty;
			if (--d->page->shared.share_count == 0)
				vm_page_release_and_dirty(d->page,
				    d->page->shared.dirty);
			ke_spinlock_exit_nospl(&d->obj->stealing_lock);
			break;

		case DEFERRED_FORKED: {
			struct vm_anon *anon = d->anon;
			vm_page_t *page = d->page;

			ke_spinlock_enter_nospl(&anon_creation_lock);
			page->shared.share_count--;

			if (--anon->refcount == 0) {
				ke_spinlock_exit_nospl(&anon_creation_lock);
				vm_page_delete(page, true);
				kmem_free(anon, sizeof(struct vm_anon));
			} else {
				ke_spinlock_exit_nospl(&anon_creation_lock);
			}
			break;
		}
		}
	}

	batch->count = 0;
}

static void
unmap_batch_add_private(struct unmap_batch *batch, vm_page_t *page)
{
	if (batch->count == UNMAP_BATCH_SIZE)
		unmap_batch_flush(batch);

	struct unmap_deferred *d = &batch->entries[batch->count++];
	d->action = DEFERRED_PRIVATE;
	d->page = page;
}

static void
unmap_batch_add_shared(struct unmap_batch *batch, vm_page_t *page,
    vm_object_t *obj, bool dirty)
{
	if (batch->count == UNMAP_BATCH_SIZE)
		unmap_batch_flush(batch);

	struct unmap_deferred *d = &batch->entries[batch->count++];
	d->action = DEFERRED_SHARED;
	d->page = page;
	d->obj = obj;
	d->dirty = dirty;
}

static void
unmap_batch_add_forked(struct unmap_batch *batch, vm_page_t *page,
    struct vm_anon *anon)
{
	if (batch->count == UNMAP_BATCH_SIZE)
		unmap_batch_flush(batch);

	struct unmap_deferred *d = &batch->entries[batch->count++];
	d->action = DEFERRED_FORKED;
	d->page = page;
	d->anon = anon;
}

static void
unmap_ptes(vm_map_t *map, vaddr_t start, vaddr_t end,
    struct vm_map_entry *entry)
{
	struct pte_cursor cursor;
	pte_t *ppte = NULL;
	ipl_t ipl;
	size_t n_zeroed = 0;
	vm_page_t *table_page = NULL;
	struct unmap_batch batch;
	int r;

	unmap_batch_init(&batch);

	ipl = spldisp();
	ke_spinlock_enter_nospl(&map->creation_lock);
	ke_spinlock_enter_nospl(&map->stealing_lock);

	for (vaddr_t addr = start; addr < end; addr += PGSIZE) {
		pte_t pte;

		if (ppte == NULL || ((uintptr_t)(++ppte) & (PGSIZE - 1)) == 0) {
			if (table_page != NULL) {
				/* commit accounting for previous table */
				if (!entry->is_phys) {
					table_page->proctable
					    .valid_pageable_leaf_ptes -=
					    n_zeroed;
					if (table_page->proctable
						.valid_pageable_leaf_ptes ==
					    0) {
						TAILQ_REMOVE(
						    &map->rs.active_leaf_tables,
						    table_page, qlink);
					}
				}
				pmap_valid_ptes_zeroed(&map->rs, table_page,
				    n_zeroed);
				pmap_unwire_pte(map, &map->rs, &cursor);
				n_zeroed = 0;
			}

			r = pmap_wire_pte(map, &map->rs, &cursor, addr, false);
			if (r < 0) {
				vaddr_t align = PGSIZE;
				switch (-r) {
#if PMAP_LEVELS >= 4
				case 3:
					align *= PMAP_L2_SKIP; /* fallthrough */
#endif
#if PMAP_LEVELS >= 3
				case 2:
					align *= PMAP_L1_SKIP; /* fallthrough */
#endif
				case 1:
					align *= PMAP_L0_SKIP;
					break;
				default:
					kfatal("Unexpected offset %d\n", -r);
				}
				/* -PGSIZE because loop adds */
				addr = roundup2(addr + 1, align) - PGSIZE;
				ppte = NULL;
				table_page = NULL;
				continue;
			}

			ppte = cursor.pte;
			table_page = cursor.pages[0];
		}

		pte = pmap_load_pte(ppte);

		switch (pmap_pte_characterise(pte)) {
		case kPTEKindZero:
			/* nothing to do */
			break;

		case kPTEKindHW: {
			vm_page_t *page;
			bool dirty;

			if (entry->is_phys) {
				/* physical mapping: just zero the PTE */
				pmap_pte_zeroleaf_create(ppte, PMAP_L0);
				n_zeroed++;
				break;
			}

			page = pmap_pte_hwleaf_page(pte, PMAP_L0);
			dirty = pmap_pte_hwleaf_writeable(pte);

			pmap_pte_zeroleaf_create(ppte, PMAP_L0);

			switch (page->use) {
			case VM_PAGE_PRIVATE:
				map->rs.private_pages_n--;
				n_zeroed++;
				unmap_batch_add_private(&batch, page);
				break;

			case VM_PAGE_ANON_FORKED: {
				unmap_batch_add_forked(&batch, page,
				    page->owner_anon);
				n_zeroed++;
				break;
			}

			case VM_PAGE_FILE:
			case VM_PAGE_ANON_SHARED: {
				unmap_batch_add_shared(&batch, page,
				    page->owner_obj, dirty);
				n_zeroed++;
				break;
			}

			default:
				kfatal("unmap_ptes: unexpected page use %d\n",
				    page->use);
			}

			map->rs.valid_n--;
			break;
		}

#if 0
		case kPTEKindTrans: {
			vm_page_t *page = pmap_pte_soft_page(pte);
			/*
			 * the only trans PTEs to be found are for process
			 * private memory (be it data or page table pages)
			 * make sure that's so.
			 */
			kassert(page->use == VM_PAGE_PRIVATE);
			pmap_pte_hw_create_zero(pte);
			vm_page_delete(page, true);
			n_zeroed++;
			break;
		}

		case kPTEKindSwap: {
			/* todo: free swap slot */
			kfatal("unmap_ptes: swap PTE - implement me\n");
			pmap_pte_hw_create_zero(pte);
			n_zeroed++;
			break;
		}

		case kPTEKindFork: {
			struct vm_anon *anon = pmap_pte_anon(pte);

			/* fork PTE: decrement anon refcount */

			ke_spinlock_enter_nospl(&anon_creation_lock);
			if (--anon->refcount == 0) {
				/* last reference to this anon; free the page */
				switch (pmap_pte_characterise(&anon->pte)) {
				case kPTEKindHW: {
					vm_page_t *page =
					    pmap_pte_hw_page(&anon->pte, 0);
					vm_page_delete(page, true);
					break;
				}
				case kPTEKindSwap:
					/* TODO: free swap slot */
					break;
				default:
					break;
				}
				kmem_free(anon, sizeof(struct vm_anon));
			}
			ke_spinlock_exit_nospl(&anon_creation_lock);

			pmap_pte_hw_create_zero(pte);
			/* fork PTEs are NOT noswap */
			table_page->proctable.nonzero_ptes--;
			break;
		}
#endif

		case kPTEKindBusy:
			/* should be impossible with rwlock held write .... */
			kfatal("impossible");

		default:
			kfatal("unexpected pte kind");
	}
	}

	/* commit accounting for the final table */
	if (table_page != NULL) {
		if (!entry->is_phys) {
			table_page->proctable.valid_pageable_leaf_ptes -=
			    n_zeroed;
			if (table_page->proctable.valid_pageable_leaf_ptes ==
			    0 && n_zeroed > 0) {
				TAILQ_REMOVE(&map->rs.active_leaf_tables,
				    table_page, qlink);
			}
		}
		pmap_valid_ptes_zeroed(&map->rs, table_page, n_zeroed);
		pmap_unwire_pte(map, &map->rs, &cursor);
	}

	ke_spinlock_exit_nospl(&map->stealing_lock);
	ke_spinlock_exit_nospl(&map->creation_lock);

	unmap_batch_flush(&batch);

	splx(ipl);
}

int
vm_unmap(struct vm_map *map, vaddr_t start, vaddr_t end)
{
	struct vm_map_entry *entry, *tmp;

	ke_rwlock_enter_write(&map->map_lock, "vm_unmap:map_lock");

	RB_FOREACH_SAFE(entry, vm_map_tree, &map->entries, tmp) {
		if ((entry->start < start && entry->end <= start) ||
		    (entry->start >= end)) {
			continue;

		} else if (entry->start >= start && entry->end <= end) {
			/* entry wholly encompassed */
			int r;

			unmap_ptes(map, entry->start, entry->end, entry);

			r = vmem_xfree(&map->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			RB_REMOVE(vm_map_tree, &map->entries, entry);

			if (entry->object != NULL)
				/* TODO retain object */
				;

			kmem_free(entry, sizeof(struct vm_map_entry));

		} else if (entry->start < start && entry->end > start &&
		    entry->end <= end) {
			/* right side of entry encompassed */
			int r;
			vaddr_t old_end = entry->end;
			vmem_addr_t new_start = entry->start;

			unmap_ptes(map, start, old_end, entry);

			r = vmem_xfree(&map->vmem, entry->start,
			    old_end - entry->start, 0);
			kassert(r == old_end - entry->start);

			entry->end = start;

			r = vmem_xalloc(&map->vmem, entry->end - new_start, 0,
			    0, 0, new_start, 0, VM_EXACT, &new_start);
			kassert(r == 0);

		} else if (entry->start >= start && entry->start < end &&
		    entry->end > end) {
			/* left side of entry encompassed */
			int r;
			vaddr_t old_start = entry->start;

			unmap_ptes(map, old_start, end, entry);

			r = vmem_xfree(&map->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			entry->start = end;
			if (entry->object != NULL)
				entry->offset += (end - old_start);

			r = vmem_xalloc(&map->vmem, entry->end - entry->start,
			    0, 0, 0, entry->start, 0, VM_EXACT, &entry->start);
			kassert(r == 0);

		} else if (entry->start < start && entry->end > end) {
			/* middle of entry encompassed - need to split */
			struct vm_map_entry *right_entry;
			int r;

			unmap_ptes(map, start, end, entry);

			r = vmem_xfree(&map->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			right_entry = kmem_alloc(sizeof(struct vm_map_entry));
			*right_entry = *entry;
			right_entry->start = end;
			if (entry->object != NULL)
				right_entry->offset = entry->offset +
				    (end - entry->start);

			/* original entry becomes left part */
			entry->end = start;

			r = vmem_xalloc(&map->vmem, entry->end - entry->start,
			    0, 0, 0, entry->start, 0, VM_EXACT, &entry->start);
			kassert(r == 0);

			r = vmem_xalloc(&map->vmem,
			    right_entry->end - right_entry->start, 0, 0, 0,
			    right_entry->start, 0, VM_EXACT,
			    &right_entry->start);
			kassert(r == 0);

			if (entry->object != NULL)
				/* TODO release object */
				;

			RB_INSERT(vm_map_tree, &map->entries, right_entry);
		}
	}

	ke_rwlock_exit_write(&map->map_lock);

	return 0;
}

/*
 * virtual object address
 */

int
vm_voaddr_acquire(struct vm_map *map, vaddr_t vaddr, struct vm_voaddr *out)
{
	struct vm_map_entry *entry;

	ke_rwlock_enter_read(&map->map_lock, "vm_voaddr:map_lock");

	entry = vm_map_lookup(map, vaddr);
	if (entry == NULL) {
		ke_rwlock_exit_read(&map->map_lock);
		return -EFAULT;
	}

	if (entry->cow || entry->object == NULL) {
		/* for cow, could go for the underlying mapping if read-only */
		out->private = true;
		out->object = (uintptr_t)map;
		out->offset = vaddr;
	} else {
		out->private = false;
		out->object = (uintptr_t)entry->object;
		out->offset = entry->offset + (vaddr - entry->start);
	}

	ke_rwlock_exit_read(&map->map_lock);

	return 0;
}
void
vm_voaddr_release(struct vm_map *, struct vm_voaddr *)
{
	/* we don't actually ref anything yet */
}

intptr_t
vm_voaddr_cmp(const struct vm_voaddr *a, const struct vm_voaddr *b)
{
	if (a->object < b->object)
		return -1;
	else if (a->object > b->object)
		return 1;
	else if (a->offset < b->offset)
		return -1;
	else if (a->offset > b->offset)
		return 1;
	else if (a->private < b->private)
		return -1;
	else if (a->private > b->private)
		return 1;
	else
		return 0;
}
