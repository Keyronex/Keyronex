/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024 Cloudarox Solutions.
 */
/*!
 * @file phys.c
 * @brief Physical memory management.
 */

#include <keyronex/dlog.h>
#include <keyronex/ktypes.h>
#include <keyronex/pmap.h>

#include <libkern/lib.h>

#include "vm/page.h"

struct vm_affinity {
	paddr_t base;
	paddr_t limit;
	uint8_t domain;
};

vm_page_t *vm_pages = (vm_page_t *)RPT_BASE;

vm_domain_t vm_domains[1];
size_t highest_domid = 0;

struct vm_affinity affinities[1] = { { 0, UINTPTR_MAX, 0 } };
size_t naffinities = 1;

uint32_t
log2(uint32_t val)
{
	if (val == 0)
		return 0;
	return 32 - __builtin_clz(val - 1);
}

size_t
vm_npages_to_order(size_t npages)
{
	size_t order = log2(npages);
	if ((1 << order) < npages)
		order++;
	return order;
}

size_t
vm_bytes_to_order(size_t bytes)
{
	return vm_npages_to_order(roundup2(bytes, PGSIZE) / PGSIZE);
}

vaddr_t
vm_page_hhdm_addr(vm_page_t *page)
{
	return p2v((page - vm_pages) << PGSHIFT);
}

paddr_t
vm_page_paddr(vm_page_t *page)
{
	return (page - vm_pages) << PGSHIFT;
}

void
vmp_region_add(paddr_t base, paddr_t limit)
{
	if (base == limit)
		return;

	for (size_t aff_i = 0; aff_i < naffinities; aff_i++) {
		struct vm_affinity *aff = &affinities[aff_i];
		struct vm_domain *dom = &vm_domains[aff->domain];
		paddr_t region_base, region_limit;

		if (limit <= aff->base || base >= aff->limit)
			continue;

		if (aff->domain > highest_domid)
			highest_domid = aff->domain;

		region_base = MAX2(base, aff->base);
		region_limit = MIN2(limit, aff->limit);

		kdprintf("vm_region_add: 0x%zx-0x%zx (%zu kib; domid %d)\n",
		    region_base, region_limit,
		    (region_limit - region_base) / 1024, aff->domain);

		for (paddr_t i = region_base; i < region_limit; i += PGSIZE) {
			pfn_t pfn = i >> PGSHIFT;
			vm_page_t *page = &vm_pages[pfn];
			size_t order = MIN2(FREELIST_ORDERS - 1,
			    __builtin_ctz(pfn));

			while ((i + (1 << order) * PGSIZE) > region_limit)
				order--;

			page->domain = aff->domain;
			page->order = order;
			page->on_freelist = false;
		}

		for (paddr_t i = region_base; i < region_limit;) {
			vm_page_t *page = &vm_pages[i >> PGSHIFT];
			TAILQ_INSERT_HEAD(&dom->free_q[page->order], page,
			    qlink);
			dom->free_n[page->order]++;
			i += (1 << page->order) * PGSIZE;
			page->on_freelist = true;
			page->max_order = page->order;
		}

		dom->use_n[VM_PAGE_FREE] += (region_limit - region_base) /
		    PGSIZE;
	}
}

void
vmp_page_unfree(vm_page_t *page, size_t order)
{
	vm_page_t *initial = page;
	struct vm_domain *dom = &vm_domains[page->domain];

	/*
	 * Find the first page on a freelist in the block of pages this page is
	 * part of.
	 */
	while (!initial->on_freelist) {
		/* Let initial be the next lower power-of-2 aligned page. */
		initial -= 1 << initial->order;
	}

	/*
	 * Loop splitting initial, putting the divided two pages onto the
	 * freelist, and letting initial = whichever one the page we want to
	 * free is greater or equal to, until initial's order is the order we
	 * want to free.
	 */
	kassert_dbg(initial->order >= order, "bad order in vm_page_unfree");
	while (initial->order != order) {
		vm_page_t *second = initial + (1 << (initial->order - 1));
		size_t new_order;

		new_order = second->order;
		kassert_dbg(new_order == initial->order - 1, "vm_page_unfree");
		kassert_dbg(initial->on_freelist, "vm_page_unfree");
		kassert_dbg(!second->on_freelist, "vm_page_unfree");

		dom->free_n[initial->order]--;
		dom->free_n[new_order] += 2;

		second->on_freelist = true;
		TAILQ_INSERT_HEAD(&dom->free_q[new_order], second, qlink);

		TAILQ_REMOVE(&dom->free_q[initial->order], initial, qlink);

		initial->order = new_order;
		TAILQ_INSERT_HEAD(&dom->free_q[new_order], initial, qlink);

		if (second <= page)
			initial = second;
	}

	kassert_dbg(initial == page, "vm_page_unfree");
	kassert_dbg(initial->on_freelist, "vm_page_unfree");

	dom->free_n[order]--;
	TAILQ_REMOVE(&dom->free_q[order], page, qlink);
	initial->on_freelist = false;
}

void
vmp_range_unfree(paddr_t base, paddr_t limit)
{
	base = rounddown2(base, PGSIZE);
	limit = roundup2(limit, PGSIZE);

	for (size_t aff_i = 0; aff_i < naffinities; aff_i++) {
		struct vm_affinity *aff = &affinities[aff_i];
		paddr_t region_base, region_limit;

		if (limit <= aff->base || base >= aff->limit)
			continue;

		region_base = MAX2(base, aff->base);
		region_limit = MIN2(limit, aff->limit);

		for (paddr_t i = region_base; i < region_limit;) {
			vm_page_t *page = VM_PAGE_FOR_PADDR(i);
			size_t order = MIN2(FREELIST_ORDERS - 1,
			    __builtin_ctz(i / PGSIZE));

			while ((i + (1 << order) * PGSIZE) > region_limit)
				order--;

			vmp_page_unfree(page, order);
			i += (1 << order) * PGSIZE;
		}
	}
}

static int
dom_page_alloc(vm_domain_t *dom, vm_page_t **out, size_t order,
    enum vm_page_use use, enum vm_alloc_flags flags)
{
	size_t npages = 1 << order;
	size_t desired_order = order;
	vm_page_t *page;

	kassert(order < FREELIST_ORDERS, "bad order");

	while (TAILQ_EMPTY(&dom->free_q[order])) {
		kassert(dom->free_n[order] == 0, "domain freelist mismatch");
		if (++order == FREELIST_ORDERS)
			kfatal("dom_page_alloc: out of pages\n");
	}

	while (order != desired_order) {
		vm_page_t *page = TAILQ_FIRST(&dom->free_q[order]), *buddy;
		buddy = page + (1 << page->order) / 2;

		kassert_dbg(buddy->domain == page->domain,
		    "buddy different domain");
		kassert_dbg(buddy->order == page->order - 1,
		    "buddy wrong order");

		TAILQ_REMOVE(&dom->free_q[order], page, qlink);
		TAILQ_INSERT_HEAD(&dom->free_q[order - 1], page, qlink);
		TAILQ_INSERT_HEAD(&dom->free_q[order - 1], buddy, qlink);

		dom->free_n[order]--;
		dom->free_n[order - 1] += 2;
		buddy->on_freelist = true;

		page->order--;
		order--;
	}

	page = TAILQ_FIRST(&dom->free_q[order]);
	TAILQ_REMOVE(&dom->free_q[order], page, qlink);
	page->on_freelist = false;
	dom->free_n[order]--;

	page->ref_count = 1;
	page->use = use;
	page->dirty = 0;

	dom->use_n[use] += npages;
	dom->active_n += npages;

	*out = page;

	return 0;
}

vm_page_t *
vm_page_alloc(vm_page_use_t use, size_t order, vm_domid_t domid,
    vm_alloc_flags_t flags)
{
	vm_domain_t *dom;
	ipl_t ipl;
	vm_page_t *page;
	int r;

	if (domid == VM_DOMID_ANY || domid == VM_DOMID_LOCAL)
		domid = 0;

	dom = &vm_domains[domid];

	ipl = ke_spinlock_enter(&dom->queues_lock);
	r = dom_page_alloc(dom, &page, order, use, 0);
	ke_spinlock_exit(&dom->queues_lock, ipl);

	if (r == 0) {
#if 1 /* detect forgetting to initialise */
		memset((void *)vm_page_hhdm_addr(page), 0x99,
		    (1 << page->order) << PGSHIFT);
#endif

		return page;
	} else
		for (size_t i = 0; i < VM_MAX_DOMAINS; i++) {
			size_t idx = (domid + i) % VM_MAX_DOMAINS;
			dom = &vm_domains[idx];

			ipl = ke_spinlock_enter(&dom->queues_lock);
			r = dom_page_alloc(dom, &page, order, use, flags);
			ke_spinlock_exit(&dom->queues_lock, ipl);

			if (r == 0) {
#if 1 /* detect forgetting to initialise */
				memset((void *)vm_page_hhdm_addr(page), 0x99,
				    (1 << page->order) << PGSHIFT);
#endif

				return page;
			}
		}

	if (flags & VM_NOFAIL)
		kfatal("out of pages\n");

	return NULL;
}

static vm_page_t *
page_buddy(vm_page_t *page)
{
	paddr_t paddr = VM_PAGE_PADDR(page) ^ ((1 << page->order) * PGSIZE);
	return VM_PAGE_FOR_PADDR(paddr);
}

static void
dom_page_free(vm_domain_t *dom, vm_page_t *page)
{
	memset((void *)vm_page_hhdm_addr(page), 0,
	    (1 << page->order) << PGSHIFT);

	while (page->order < page->max_order) {
		vm_page_t *buddy = page_buddy(page);

		if (buddy->order != page->order)
			break;
		else if (!buddy->on_freelist)
			break;

		kassert_dbg(buddy->domain == page->domain,
		    "buddy domain mismatch");

		TAILQ_REMOVE(&dom->free_q[buddy->order], buddy, qlink);
		dom->free_n[buddy->order]--;

		if (page > buddy) {
			vm_page_t *tmp = page;
			page = buddy;
			buddy = tmp;
		}

		page->order++;
	}

	TAILQ_INSERT_HEAD(&dom->free_q[page->order], page, qlink);
	dom->free_n[page->order]++;
	page->on_freelist = true;

#if 1 /* try to detect use-after-free */
	memset((void *)vm_page_hhdm_addr(page), 0x66,
	    (1 << page->order) << PGSHIFT);
#endif
}

/* page owner lock (if there is one) should be held */
void
vm_page_delete(vm_page_t *page, bool unref)
{
	vm_domain_t *dom = &vm_domains[page->domain];
	size_t npages = 1 << page->order;
	ipl_t ipl;
	uint32_t refcnt;

	ipl = ke_spinlock_enter(&dom->queues_lock);

	kassert(page->use != VM_PAGE_DELETED, "page already deleted");

	dom->use_n[page->use] -= npages;

	if (unref)
		refcnt = --page->ref_count;
	else
		refcnt = page->ref_count;

	if (refcnt == 0) {
		if (!unref) {
			/* then page must be on one of the paging queues */

			if (page->dirty) {
				TAILQ_REMOVE(&dom->dirty_q, page, qlink);
				dom->dirty_n -= npages;
			} else {
				TAILQ_REMOVE(&dom->stby_q, page, qlink);
				dom->stby_n -= npages;
			}
		} else {
			dom->active_n -= npages;
		}

		dom_page_free(dom, page);
	} else {
		dom->use_n[VM_PAGE_DELETED] += 1 << page->order;
	}

	ke_spinlock_exit(&dom->queues_lock, ipl);
}

/* page owner lock (if there is one) should be held */
void
vm_page_retain(vm_page_t *page)
{
	vm_domain_t *dom = &vm_domains[page->domain];
	size_t npages = 1 << page->order;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&dom->queues_lock);

	if (page->ref_count++ == 0) {
		/* inactive -> active state */

		if (page->use == VM_PAGE_DELETED) {
			kfatal("Attempt to retain deleted page\n");
		}

		if (page->dirty) {
			TAILQ_REMOVE(&dom->dirty_q, page, qlink);
			dom->dirty_n -= npages;
		} else {
			TAILQ_REMOVE(&dom->stby_q, page, qlink);
			dom->stby_n -= npages;
		}

		dom->active_n += npages;
	}

	ke_spinlock_exit(&dom->queues_lock, ipl);
}

void
vm_page_release_dom_locked(vm_page_t *page)
{
	vm_domain_t *dom = &vm_domains[page->domain];
	size_t npages = 1 << page->order;

	if (page->ref_count -- == 1) {
		/* active -> inactive state */

		switch (page->use) {
		case VM_PAGE_DELETED:
		case VM_PAGE_DEV_BUFFER:
		case VM_PAGE_KWIRED:
			dom->active_n -= npages;
			dom->use_n[page->use] -= npages;
			dom_page_free(dom, page);
			break;

		case VM_PAGE_PRIVATE:
		case VM_PAGE_ANON_SHARED:
		case VM_PAGE_FILE:
		case VM_PAGE_TABLE:
		case VM_PAGE_OBJ_TABLE:
		case VM_PAGE_ANON_FORKED:
			dom->active_n -= npages;

			if (page->dirty) {
				TAILQ_INSERT_TAIL(&dom->dirty_q, page, qlink);
				dom->dirty_n += npages;
			} else {
				TAILQ_INSERT_TAIL(&dom->stby_q, page, qlink);
				dom->stby_n += npages;
			}

			break;

		default:
			kfatal("Attempt to free page in unknown state %d\n",
			    page->use);
		}
	}
}

void
vm_page_release(vm_page_t *page)
{
	vm_domain_t *dom = &vm_domains[page->domain];
	ipl_t ipl = ke_spinlock_enter(&dom->queues_lock);
	vm_page_release_dom_locked(page);
	ke_spinlock_exit(&dom->queues_lock, ipl);
}

void
vm_page_release_and_dirty(vm_page_t *page, bool dirty)
{
	vm_domain_t *dom = &vm_domains[page->domain];
	ipl_t ipl = ke_spinlock_enter(&dom->queues_lock);
	if (dirty) {
#if 0
		if (!page->dirty && page->use == VM_PAGE_FILE)
			vn_retain(page->owner_obj->vnode);
#endif

		page->dirty = 1;
	}
	vm_page_release_dom_locked(page);
	ke_spinlock_exit(&dom->queues_lock, ipl);
}

void
vm_page_dirty(vm_page_t *page)
{
	vm_domain_t *dom = &vm_domains[page->domain];
	ipl_t ipl = ke_spinlock_enter(&dom->queues_lock);

	/* this function isn't for pages on paging queues */
	kassert(page->ref_count > 0, "vm_pageD_irty: page refcount = 0");

	if (!page->dirty) {
#if 0
		if (page->use == VM_PAGE_FILE)
			vn_retain(page->owner_obj->vnode);
#endif

		page->dirty = 1;
	}

	ke_spinlock_exit(&dom->queues_lock, ipl);
}

static vm_page_t *
vm_page_steal(vm_page_use_t use, vm_domain_t *dom)
{
#if 0 /* not ready */
	vm_page_t *page;
	kspinlock_t *owner_lock;
	pte_t *ppte;
	union {
		struct vm_rs *rs;
		struct vm_object *obj;
	} owner;
	enum vm_page_use old_use;

	kassert(ke_spinlock_held(&dom->queues_lock),
	    "domain queues lock must be held");

retry:
	page = TAILQ_FIRST(&dom->stby_q);
	if (page == NULL)
		return NULL;

	TAILQ_REMOVE(&dom->stby_q, page, qlink);

	kassert_dbg(page->ref_count == 0, "page refcount not 0");
	kassert_dbg(page->order == 0, "page order not 0");
	kassert_dbg(!page->dirty, "page dirty");

	switch (page->use) {
#if 0
	case VM_PAGE_TABLE:
	case VM_PAGE_PRIVATE:
		owner.rs = page->owner_rs;
		owner_lock = &owner.rs->map->stealing_lock;
		break;

	case VM_PAGE_OBJ_TABLE:
	case VM_PAGE_ANON_SHARED:
	case VM_PAGE_FILE:
		owner.obj = page->owner_obj;
		owner_lock = &owner.obj->stealing_lock;
		break;
#endif

	default:
		kfatal("Pages of kind %d should not be on standby queue\n",
		    page->use);
	}
	old_use = page->use;

	page->ref_count++;
	ppte = page->pte;

	dom->stby_n -= 1 << page->order;
	dom->active_n += 1 << page->order;

	ke_spinlock_exit_nospl(&dom->queues_lock);

	/*
	 * We can safely acquire the owner's lock, because VM objects and RS
	 * structures are RCU type-stable.
	 */

	ke_spinlock_enter_nospl(owner_lock);
	ke_spinlock_enter_nospl(&dom->queues_lock);

	if (page->ref_count != 1 || page->pte != ppte || page->use != old_use) {
		kdprintf("note: page changed before it could be stolen\n");
		vm_page_release_dom_locked(page);
		ke_spinlock_exit_nospl(owner_lock);
		goto retry;
	}

	switch (old_use) {
	case VM_PAGE_TABLE:
		kfatal("vm_page_steal: page table page being stolen!\n");
	case VM_PAGE_PRIVATE: {
		vm_page_t *table_page = VM_PAGE_FOR_HHDM_ADDR((vaddr_t)ppte);
		pte_t pte = pmap_load_pte(ppte);
		kassert(pmap_pte_characterise(pte) == kPTEKindTrans, "page use not transitional");
		pmap_pte_create_soft(pte, kPTEKindSwap, page->swap_address,
		    false);
		/*
		 * TODO: Figure out if it's fine to drop the queues lock - I
		 * think it is!
		 *
		 * We drop it because the did-become-zero/swap cases sometimes
		 * delete table pages.
		 */
		ke_spinlock_exit_nospl(&dom->queues_lock);
		pmap_table_pte_did_become_swap(table_page);
		break;
	}

	case VM_PAGE_OBJ_TABLE: {
		vm_page_t *table_page = VM_PAGE_FOR_HHDM_ADDR((vaddr_t)pte);
		kassert(pmap_pte_characterise(pte) == kPTEKindTrans);
		pmap_pte_create_soft(pte, kPTEKindSwap, page->swap_address,
		    false);
		ke_spinlock_exit_nospl(&dom->queues_lock);
		obj_table_pte_did_become_swap(owner.obj, table_page);
		break;
	}

	case VM_PAGE_ANON_SHARED: {
		kassert(pmap_pte_characterise(pte) == kPTEKindHW);
		pmap_pte_create_soft(page->pte, kPTEKindSwap,
		    page->swap_address, false);
		ke_spinlock_exit_nospl(&dom->queues_lock);
		obj_page_swapped(owner.obj, page);
		break;
	}

	case VM_PAGE_FILE: {
		kassert(pmap_pte_characterise(pte) == kPTEKindHW);
		pmap_pte_hw_create_zero(pte);
		ke_spinlock_exit_nospl(&dom->queues_lock);
		obj_page_zeroed(owner.obj, page);
		break;
	}

	default:
		kfatal("Handle these\n");
	}

	ke_spinlock_exit_nospl(owner_lock);
	ke_spinlock_enter_nospl(&dom->queues_lock);

	dom->use_n[old_use] -= 1 << page->order;
	dom->use_n[use] += 1 << page->order;

	page->use = use;
	page->dirty = 0;

	return page;
#endif
	kfatal("vm_page_steal: needs to be adapted");
}

void
vm_purge_standby(void)
{
	ipl_t ipl = spldisp();
	for (size_t dom_i = 0; dom_i <= highest_domid; dom_i++) {
		vm_domain_t *dom = &vm_domains[dom_i];
		ke_spinlock_enter_nospl(&dom->queues_lock);
		vm_page_t *page;
		while ((page = vm_page_steal(VM_PAGE_KWIRED, dom)) != NULL) {
			ke_spinlock_exit_nospl(&dom->queues_lock);
			vm_page_delete(page, false);
			ke_spinlock_enter_nospl(&dom->queues_lock);
		}
		ke_spinlock_exit_nospl(&dom->queues_lock);
	}
	splx(ipl);
}
