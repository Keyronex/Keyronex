/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Aug 11 2023.
 */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 *  License, v. 2.0. If a copy of the MPL was not distributed with this
 *  file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*!
 * @file page.c
 * @brief Resident page management - allocation, deallocation, etc.
 */

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "vmp.h"

#define KRX_VM_SANITY_CHECKING
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

#if KRX_PORT_BITS == 64
#define BAD_INT 0xdeadbeefdeafbeef
#else
#define BAD_INT 0xdeadbeef
#endif
#define BAD_PTR ((void *)BAD_INT)

#define DEFINE_PAGEQUEUE(NAME) \
	static page_queue_t NAME = TAILQ_HEAD_INITIALIZER(NAME)

typedef TAILQ_HEAD(, vm_page) page_queue_t;

struct vmp_pregion {
	/*! Linkage to pregion_queue. */
	TAILQ_ENTRY(vmp_pregion) queue_entry;
	/*! Base address of region. */
	paddr_t base;
	/*! Number of pages the region covers. */
	size_t npages;
	/*! PFN database part for region. */
	vm_page_t pages[0];
};

#define BUDDY_ORDERS 16
static page_queue_t buddy_queue[BUDDY_ORDERS];
static size_t buddy_queue_npages[BUDDY_ORDERS];
DEFINE_PAGEQUEUE(vm_pagequeue_modified);
DEFINE_PAGEQUEUE(vm_pagequeue_standby);
static TAILQ_HEAD(, vmp_pregion) pregion_queue = TAILQ_HEAD_INITIALIZER(
    pregion_queue);
struct vm_stat vmstat;
kspinlock_t    vmp_pfn_lock = KSPINLOCK_INITIALISER;
vm_account_t   deleted_account, general_account;

static inline void
update_page_use_stats(enum vm_page_use use, int value)
{
	kassert(ke_spinlock_held(&vmp_pfn_lock));

#define CASE(ENUM, VAR)              \
	case ENUM:                   \
		vmstat.VAR += value; \
		break

	switch (use) {
		CASE(kPageUseDeleted, ndeleted);
		CASE(kPageUseAnonPrivate, nanonprivate);
		CASE(kPageUseKWired, nkwired);
		CASE(kPageUsePML3, nprocpgtable);
		CASE(kPageUsePML2, nprocpgtable);
		CASE(kPageUsePML1, nprocpgtable);

	default:
		kfatal("Handle\n");
	}
}

#define MDL_SIZE(NPAGES) (sizeof(vm_mdl_t) + sizeof(vm_mdl_entry_t) * NPAGES)

vm_mdl_t*
vm_mdl_buffer_alloc(size_t npages)
{
	vm_mdl_t *mdl = kmem_alloc(MDL_SIZE(npages));
	mdl->nentries = npages;
	for (unsigned i = 0; i < npages; i++) {
		vm_page_t *page;
		int r = vm_page_alloc(&page, 0, &general_account,
		    kPageUseKWired, false);
		kassert(r == 0);
		mdl->entries[i].paddr = vm_page_paddr(page);
		mdl->entries[i].bytes = PGSIZE;
	}
	return mdl;
}

paddr_t
mdl_translate(vaddr_t vaddr, bool paged)
{
	if (vaddr >= HHDM_BASE && vaddr <= (HHDM_BASE + HHDM_SIZE)) {
		paddr_t paddr;
		kassert(!paged);
		paddr = V2P(vaddr);
		/* vm_page_retain panics if page is NULL */
		vm_page_retain(vm_paddr_to_page(PGROUNDDOWN(paddr)),
		    &general_account);
		return paddr;
	} else {
		ipl_t ipl = vmp_acquire_pfn_lock();
		paddr_t paddr;
		paddr = vmp_md_translate(vaddr);
		vmp_page_retain_locked(vm_paddr_to_page(PGROUNDDOWN(paddr)),
		    &general_account);
		vmp_release_pfn_lock(ipl);
		return paddr;
	}
}

vm_mdl_t *
vm_mdl_create(void *address, size_t size)
{
	vm_mdl_t *mdl;
	vaddr_t vaddr = (vaddr_t)address;
	size_t nentries;

	nentries = (ROUNDUP(vaddr + size, PGSIZE) - ROUNDDOWN(vaddr, PGSIZE)) /
	    PGSIZE;
	mdl = kmem_alloc(sizeof(vm_mdl_t) + nentries * sizeof(vm_mdl_entry_t));

	if (mdl == NULL)
		return NULL;

	mdl->nentries = nentries;

	kprintf("-- mdl_create(%zu, %zu)\n", vaddr, size);

	for (size_t i = 0; i < nentries; ++i) {
		paddr_t phys_addr = mdl_translate(vaddr, false);
		size_t entry_bytes = MIN2(PGSIZE, size);

		kprintf("entry_bytes is initially %zu - size is %zu\n",
		    entry_bytes, size);
		if (i == 0 && entry_bytes == PGSIZE)
			entry_bytes -= vaddr % PGSIZE;

		kprintf("entry_bytes is now %zu\n", entry_bytes);

		mdl->entries[i].paddr = phys_addr;
		mdl->entries[i].bytes = entry_bytes;
		kprintf("entry_bytes is finally %zu\n", entry_bytes);
		kprintf(" -- %d: %zu/%zu\n", i, phys_addr, entry_bytes);

		vaddr = vaddr + entry_bytes;
		size -= entry_bytes;
	}

	return mdl;
}

paddr_t
vm_mdl_paddr(vm_mdl_t *mdl, voff_t offset)
{
	size_t remaining = (size_t)offset;

	for (size_t i = 0; i < mdl->nentries; ++i) {
		if (remaining < mdl->entries[i].bytes) {
			return mdl->entries[i].paddr + remaining;
		} else {
			remaining -= mdl->entries[i].bytes;
		}
	}

	kfatal("out of bounds");
}

void
vm_region_add(paddr_t base, size_t length)
{
	struct vmp_pregion *bm = (void *)P2V(base);
	size_t		    used; /* n bytes used by bitmap struct */
	int		    b;
	paddr_t limit;

	/* set up a pregion for this area */
	bm->base = base;
	bm->npages = length / PGSIZE;
	limit = bm->base + length;

	used = ROUNDUP(sizeof(struct vmp_pregion) +
		sizeof(vm_page_t) * bm->npages,
	    PGSIZE);

	kprintf("vm_region_add: 0x%zx-0x%zx"
		"(%zu MiB, %zu pages; PFDB part %zu kib)\n",
	    base, base + length, length / (1024 * 1024), length / PGSIZE, used / 1024);

	/* initialise pages */
	for (b = 0; b < bm->npages; b++) {
		bm->pages[b].pfn = PADDR_TO_PFN(bm->base + PGSIZE * b);
		// bm->pages[b].anon = NULL;
		// LIST_INIT(&bm->pages[b].pv_list);
	}

	/* mark off the pages used */
	for (b = 0; b < used / PGSIZE; b++) {
		bm->pages[b].use = kPageUsePFNDB;
		bm->pages[b].refcnt = 1;
		vmstat.npwired++;
	}

	/* now zero the remainder */
	for (size_t i = b; i < bm->npages; i++) {
		vm_page_t *page = &bm->pages[i];
		paddr_t paddr = vm_page_paddr(page);
		size_t order = MIN2(BUDDY_ORDERS - 1,
		    __builtin_ctz(paddr / PGSIZE));
		if ((paddr + (1 << order) * PGSIZE) > limit)
			order = MIN2(BUDDY_ORDERS - 1,
			    __builtin_ctz((limit - paddr) / PGSIZE));
		page->order = order;
		page->refcnt = 0;
		page->used_ptes = 0;
		page->referent_pte = 0;
		page->use = kPageUseFree;
		page->on_freelist = false;
	}

	for (size_t i = b; i < bm->npages;) {
		vm_page_t *page = &bm->pages[i];
		TAILQ_INSERT_HEAD(&buddy_queue[page->order], page, queue_link);
		buddy_queue_npages[page->order]++;
		i += (1 << page->order);
		page->on_freelist = true;
	}

	vmstat.nfree += bm->npages - (used / PGSIZE);
	// vmstat.ntotal += bm->npages;

	TAILQ_INSERT_TAIL(&pregion_queue, bm, queue_entry);
}

vm_page_t *
vm_paddr_to_page(paddr_t paddr)
{
	struct vmp_pregion *preg;

	TAILQ_FOREACH (preg, &pregion_queue, queue_entry) {
		if (preg->base <= paddr &&
		    (preg->base + PGSIZE * preg->npages) > paddr) {
			return &preg->pages[(paddr - preg->base) / PGSIZE];
		}
	}

	kfatal("No page for paddr 0x%zx\n", paddr);
	return NULL;
}

int
vmp_pages_alloc_locked(vm_page_t **out, size_t order, vm_account_t *account,
    enum vm_page_use use, bool must)
{
	size_t npages = vm_order_to_npages(order);
	vm_page_t *page;
	size_t desired_order = order;

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	kassert(order < BUDDY_ORDERS);

	while (TAILQ_EMPTY(&buddy_queue[order])) {
		if (++order == BUDDY_ORDERS)
			kfatal("out of pages\n");
	}

	while (order != desired_order) {
		vm_page_t *page = TAILQ_FIRST(&buddy_queue[order]), *buddy;
		buddy = page + (1 << page->order) / 2;

		kassert(buddy->order == page->order - 1);

		TAILQ_REMOVE(&buddy_queue[order], page, queue_link);
		TAILQ_INSERT_HEAD(&buddy_queue[order - 1], page, queue_link);
		TAILQ_INSERT_HEAD(&buddy_queue[order - 1], buddy, queue_link);

		buddy_queue_npages[order]--;
		buddy_queue_npages[order - 1] += 2;
		buddy->on_freelist = true;

		page->order--;
		order--;
	}

	page = TAILQ_FIRST(&buddy_queue[order]);
	TAILQ_REMOVE(&buddy_queue[order], page, queue_link);
	page->on_freelist = false;
	buddy_queue_npages[order]--;

	page->refcnt = 1;
	page->use = use;
	page->busy = 0;
	page->dirty = 0;
	page->offset = 0;
	page->referent_pte = 0;
	page->owner = 0;
	page->swap_descriptor = 0;
	page->used_ptes = 0;

	vmstat.nfree -= npages;
	vmstat.nactive += npages;
	account->nalloced += npages;
	account->nwires += npages;
	update_page_use_stats(use, npages);

	memset((void *)vm_page_direct_map_addr(page), 0x0, PGSIZE * npages);

	*out = page;
	return 0;
}

int
vmp_page_alloc_locked(vm_page_t **out, vm_account_t *account,
    enum vm_page_use use, bool must)
{
	return vmp_pages_alloc_locked(out, 0, account, use, must);
}

static void
page_free(struct vmp_pregion *preg, vm_page_t *page)
{
	while (true) {
		vm_page_t *buddy;
		intptr_t index = page - preg->pages, buddy_index;
		size_t pages = 1 << page->order;

		if (index % (2 * pages) == 0)
			buddy_index = index + pages;
		else
			buddy_index = index - pages;

		if ((size_t)buddy_index > preg->npages || buddy_index < 0)
			break;

		buddy = &preg->pages[buddy_index];

		if (buddy->order != page->order)
			break;
		else if (!buddy->on_freelist)
			break;

		TAILQ_REMOVE(&buddy_queue[buddy->order], buddy, queue_link);
		buddy_queue_npages[buddy->order]--;

		if (page > buddy) {
			vm_page_t *tmp = page;
			page = buddy;
			buddy = tmp;
		}

		page->order++;
	}

	TAILQ_INSERT_HEAD(&buddy_queue[page->order], page, queue_link);
	buddy_queue_npages[page->order]++;
	page->on_freelist = true;
}

void
vmp_page_free_locked(vm_page_t *page)
{
	struct vmp_pregion *preg;
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->use == kPageUseDeleted);
	kassert(page->refcnt == 0);

	kprintf("Freeing page %p\n", page);

	TAILQ_FOREACH (preg, &pregion_queue, queue_entry) {
		if (preg->base <= vm_page_paddr(page) &&
		    (preg->base + PGSIZE * preg->npages) >
			vm_page_paddr(page)) {

			page->dirty = false;
			page->referent_pte = 0;
			page->use = kPageUseFree;
			page->used_ptes = 0;
			deleted_account.nalloced -= npages;
			vmstat.nfree += npages;
			vmstat.ndeleted -= npages;
			return page_free(preg, page);
		}
	}
}

void
vmp_page_delete_locked(vm_page_t *page, vm_account_t *account, bool release)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->use != kPageUseDeleted);
	kassert(!page->busy);

	update_page_use_stats(page->use, -npages);
	vmstat.ndeleted += npages;
	page->use = kPageUseDeleted;

	account->nalloced -= npages;
	deleted_account.nalloced += npages;

	if (release) {
		kassert(page->refcnt > 0);
		vmp_page_release_locked(page, account);
	} else {
		if (page->refcnt == 0 && page->dirty) {
			TAILQ_REMOVE(&vm_pagequeue_modified, page, queue_link);
			vmstat.nmodified -= npages;
			vmp_page_free_locked(page);
		} else if (page->refcnt == 0) {
			TAILQ_REMOVE(&vm_pagequeue_standby, page, queue_link);
			vmstat.nstandby -= npages;
			vmp_page_free_locked(page);
		}
	}
}

vm_page_t *
vmp_page_retain_locked(vm_page_t *page, vm_account_t *account)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	account->nwires += npages;

	if (page->refcnt++ == 0) {
		/* going from inactive to active state */
		kassert(page->use != kPageUseDeleted);
		if (page->dirty) {
			TAILQ_REMOVE(&vm_pagequeue_modified, page, queue_link);
			vmstat.nmodified -= npages;
			vmstat.nactive += npages;
		} else {
			TAILQ_REMOVE(&vm_pagequeue_standby, page, queue_link);
			vmstat.nstandby -= npages;
			vmstat.nactive += npages;
		}
	}

	return page;
}

vm_page_t *
vm_page_retain(vm_page_t *page, vm_account_t *account)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	vm_page_t *ret = vmp_page_retain_locked(page, account);
	vmp_release_pfn_lock(ipl);
	return ret;
}

void
vmp_page_release_locked(vm_page_t *page, vm_account_t *account)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->refcnt > 0);

	account->nwires -= npages;

	if (page->refcnt-- == 1) {
		/* going from active to inactive state */
		vmstat.nactive -= npages;

		switch (page->use) {
		case kPageUseDeleted:
			vmp_page_free_locked(page);
			return;

		case kPageUseAnonPrivate:
			/* continue with logic below */
			break;

		default:
			kfatal("Release page of unexpected type\n");
		}

		/* this is a pageable page, so put it on the appropriate q */

		if (page->dirty) {
			TAILQ_INSERT_TAIL(&vm_pagequeue_modified, page,
			    queue_link);
			vmstat.nmodified += npages;
		} else {
			TAILQ_INSERT_TAIL(&vm_pagequeue_standby, page,
			    queue_link);
			vmstat.nstandby += npages;
		}
	}
}

static const char *
vm_page_use_str(enum vm_page_use use)
{
	switch (use) {
	case kPageUseFree:
		return "free";
	case kPageUseKWired:
		return "kwired";
	case kPageUsePFNDB:
		return "pfndb";
	case kPageUseAnonPrivate:
		return "anon-private";
	case kPageUsePML3:
		return "PML3";
	case kPageUsePML2:
		return "PML2";
	case kPageUsePML1:
		return "PML1";
	default:
		return "BAD";
	}
}

void
vmp_pages_dump(void)
{
	struct vmp_pregion *region;

	kprintf("Active: %zu, modified: %zu, standby: %zu, free: %zu\n",
	    vmstat.nactive, vmstat.nmodified, vmstat.nstandby, vmstat.nfree);

	kprintf("\033[7m%-9s%-9s%-9s%-9s%-9s\033[m\n", "free", "del", "priv",
	    "fork", "file");
	kprintf("%-9zu%-9zu%-9zu%-9zu%-9zu\n", vmstat.nfree, vmstat.ndeleted,
	    vmstat.nanonprivate, vmstat.nanonfork, vmstat.nfile);
	kprintf("\033[7m%-9s%-9s%-9s%-9s%-9s\033[m\n", "share", "pgtbl",
	    "proto", "kwired", "pwired");
	kprintf("%-9zu%-9zu%-9zu%-9zu%-9zu\n", vmstat.nanonshare,
	    vmstat.nprocpgtable, vmstat.nprotopgtable, vmstat.nkwired,
	    vmstat.npwired);

	TAILQ_FOREACH (region, &pregion_queue, queue_entry) {
		for (int i = 0; i < region->npages; i++) {
			vm_page_t *page = &region->pages[i];

			if (page->use == kPageUseFree ||
			    page->use == kPageUsePFNDB || page->use == kPageUseKWired)
				continue;
			kprintf("Page %d: Use %s, RC %d, Used-PTEs %d\n", i,
			    vm_page_use_str(page->use), page->refcnt,
			    page->used_ptes);
		}
	}
}

int
vm_page_alloc(vm_page_t **out, size_t order, vm_account_t *account,
    enum vm_page_use use, bool must)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	int r = vmp_pages_alloc_locked(out, order, account, use, must);
	vmp_release_pfn_lock(ipl);
	return r;
}