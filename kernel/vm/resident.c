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

#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "vmp.h"
#include "vmp_dynamics.h"

#define KRX_VM_SANITY_CHECKING
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

#if KRX_PORT_BITS == 64
#define BAD_INT 0xdeadbeefdeafbeef
#else
#define BAD_INT 0xdeadbeef
#endif
#define BAD_PTR ((void *)BAD_INT)

#define DEFINE_PAGEQUEUE(NAME) page_queue_t NAME = TAILQ_HEAD_INITIALIZER(NAME)

void vmp_page_steal(vm_page_t *page, enum vm_page_use new_use);

#define BUDDY_ORDERS 16
static page_queue_t buddy_queue[BUDDY_ORDERS];
static size_t buddy_queue_npages[BUDDY_ORDERS];
DEFINE_PAGEQUEUE(vm_pagequeue_modified);
DEFINE_PAGEQUEUE(vm_pagequeue_standby);
struct vm_stat vmstat;
paddr_t vmp_highest_address;
vm_page_t *pfndb = (vm_page_t *)PFNDB_BASE;
kspinlock_t vmp_pfn_lock = KSPINLOCK_INITIALISER;

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
		CASE(kPageUseAnonShared, nanonshare);
		CASE(kPageUseAnonPrivate, nanonprivate);
		CASE(kPageUseFileShared, nfileshared);
		CASE(kPageUseKWired, nkwired);
		CASE(kPageUsePML4, nprocpgtable);
		CASE(kPageUsePML3, nprocpgtable);
		CASE(kPageUsePML2, nprocpgtable);
		CASE(kPageUsePML1, nprocpgtable);
		CASE(kPageUseVPML4, nprotopgtable);
		CASE(kPageUseVPML3, nprotopgtable);
		CASE(kPageUseVPML2, nprotopgtable);
		CASE(kPageUseVPML1, nprotopgtable);

	default:
		kfatal("Handle\n");
	}
}

#define MDL_SIZE(NPAGES) (sizeof(vm_mdl_t) + sizeof(vm_mdl_entry_t) * NPAGES)

vm_mdl_t *
vm_mdl_alloc_with_pages(size_t npages, enum vm_page_use use, bool pfnlock_held)
{
	vm_mdl_t *mdl = kmem_alloc(MDL_SIZE(npages));
	mdl->nentries = npages;
	mdl->offset = 0;
	for (unsigned i = 0; i < npages; i++) {
		vm_page_t *page;
		int r;

		r = (pfnlock_held ? vmp_pages_alloc_locked :
				    vm_page_alloc)(&page, 0, use, false);
		kassert(r == 0);
		mdl->pages[i] = page;
	}
	return mdl;
}

vm_mdl_t *
vm_mdl_buffer_alloc(size_t npages)
{
	return vm_mdl_alloc_with_pages(npages, kPageUseKWired, false);
}

vm_page_t *
mdl_translate(vaddr_t vaddr, bool paged)
{
	vm_page_t *page;
	if (vaddr >= HHDM_BASE && vaddr <= (HHDM_BASE + HHDM_SIZE)) {
		paddr_t paddr;
		kassert(!paged);
		paddr = (paddr_t)V2P(vaddr);
		/* vm_page_retain panics if page is NULL */
		page = vm_page_retain(vm_paddr_to_page(PGROUNDDOWN(paddr)));
		return page;
	} else {
		ipl_t ipl = vmp_acquire_pfn_lock();
		paddr_t paddr;
		paddr = vmp_md_translate(vaddr);
		page = vmp_page_retain_locked(
		    vm_paddr_to_page(PGROUNDDOWN(paddr)));
		vmp_release_pfn_lock(ipl);
		return page;
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

	mdl->offset = (uintptr_t)address % PGSIZE;
	mdl->nentries = nentries;

	for (size_t i = 0; i < nentries; ++i) {
		vm_page_t *page = mdl_translate(vaddr, false);
		size_t entry_bytes = MIN2(PGSIZE, size);

		if (i == 0 && entry_bytes == PGSIZE)
			entry_bytes -= vaddr % PGSIZE;

		mdl->pages[i] = page;

		vaddr = vaddr + entry_bytes;
		size -= entry_bytes;
	}

	return mdl;
}

paddr_t
vm_mdl_paddr(vm_mdl_t *mdl, voff_t offset)
{
	size_t total_offset = mdl->offset + offset;
	size_t page_idx = total_offset / PGSIZE;

	if (page_idx >= mdl->nentries)
		return (paddr_t)-1;

	paddr_t page_base_addr = vm_page_paddr(mdl->pages[page_idx]);
	size_t intra_page_offset = total_offset % PGSIZE;
	return page_base_addr + intra_page_offset;
}

void
vm_mdl_copy_into(vm_mdl_t *mdl, voff_t offset, void *data, size_t len)
{
	size_t written = 0;

	kassert(mdl->offset + offset + len <= mdl->nentries * PGSIZE -
	    mdl->offset);

	while (written < len) {
		paddr_t paddr = vm_mdl_paddr(mdl, offset);
		size_t limit = MIN2(PGSIZE - (paddr % PGSIZE), len);
		memcpy((void *)P2V(paddr), data + written, limit);
		written += limit;
		offset += limit;
		len -= limit;
	}
}

size_t
vm_mdl_contig_bytes(vm_mdl_t *mdl, voff_t offset)
{
	return PGSIZE - (mdl->offset + offset) % PGSIZE;
}

void
vm_region_add(paddr_t base, paddr_t limit)
{
	if (base == limit)
		return;

	kprintf("vm_region_add: 0x%zx-0x%zx (%zu kib)\n", base, limit,
	    (limit - base) / 1024);

	for (paddr_t i = base; i < limit; i += PGSIZE) {
		pfn_t pfn = i >> VMP_PAGE_SHIFT;
		vm_page_t *page = &pfndb[pfn];
		size_t order = MIN2(BUDDY_ORDERS - 1, __builtin_ctz(pfn));

		if ((i + (1 << order) * PGSIZE) > limit)
			order = MIN2(BUDDY_ORDERS - 1,
			    __builtin_ctz((limit - i) / PGSIZE));

		page->order = order;
		page->refcnt = 0;
		page->nonzero_ptes = 0;
		page->referent_pte = 0;
		page->use = kPageUseFree;
		page->on_freelist = false;
		page->pfn = pfn;
	}

	for (paddr_t i = base; i < limit;) {
		vm_page_t *page = &pfndb[i / PGSIZE];
		TAILQ_INSERT_HEAD(&buddy_queue[page->order], page, queue_link);
		buddy_queue_npages[page->order]++;
		i += (1 << page->order) * PGSIZE;
		page->on_freelist = true;
#if BITS == 64
		page->max_order = page->order;
#endif
	}

	vmstat.nfree += (limit - base) / PGSIZE;
	vmstat.ntotal += (limit - base) / PGSIZE;
}

void
vmp_page_unfree(vm_page_t *page, size_t order)
{
	vm_page_t *initial = page;

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
	while (initial->order != order) {
		vm_page_t *second = initial + (1 << (initial->order - 1));
		size_t newOrder = second->order;

		kassert(newOrder == initial->order - 1);
		kassert(initial->on_freelist);
		kassert(!second->on_freelist);

		second->on_freelist = true;
		TAILQ_INSERT_HEAD(&buddy_queue[newOrder], second, queue_link);

		TAILQ_REMOVE(&buddy_queue[initial->order], initial, queue_link);

		initial->order = newOrder;
		TAILQ_INSERT_HEAD(&buddy_queue[newOrder], initial, queue_link);

		if (second <= page)
			initial = second;
	}

	kassert(initial == page);
	kassert(initial->on_freelist);

	TAILQ_REMOVE(&buddy_queue[order], page, queue_link);
	vmstat.nfree -= 1 << order;
	vmstat.npwired += 1 << order;
	initial->on_freelist = false;
}

void
vmp_range_unfree(paddr_t base, paddr_t limit)
{
	base = ROUNDDOWN(base, PGSIZE);
	limit = ROUNDUP(limit, PGSIZE);

	for (paddr_t i = base; i < limit;) {
		vm_page_t *page = vm_paddr_to_page(i);
		size_t order = MIN2(BUDDY_ORDERS - 1,
		    __builtin_ctz(i / PGSIZE));

		if ((i + (1 << order) * PGSIZE) > limit)
			order = MIN2(BUDDY_ORDERS - 1,
			    __builtin_ctz((limit - i) / PGSIZE));

		vmp_page_unfree(page, order);
		i += (1 << order) * PGSIZE;
	}
}

vm_page_t *
vm_paddr_to_page(paddr_t paddr)
{
	return &pfndb[paddr >> VMP_PAGE_SHIFT];
}

int
vmp_pages_alloc_locked(vm_page_t **out, size_t order, enum vm_page_use use,
    bool must)
{
	size_t npages = vm_order_to_npages(order);
	vm_page_t *page;
	size_t desired_order = order;

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	if (vmp_avail_pages_very_low() && !must)
		return -1;
	else if (vmp_free_pages_low()) {
		page = TAILQ_FIRST(&vm_pagequeue_standby);
		if (page == NULL && !must)
			return -1;
		else if (page != NULL) {
			TAILQ_REMOVE(&vm_pagequeue_standby, page, queue_link);
			vmp_page_steal(page, kPageUseDeleted);
		}
	}

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
	page->drumslot = 0;
	page->nonzero_ptes = 0;
	page->noswap_ptes = 0;
	page->wsi_hint = NIL_WSE;

	vmstat.nfree -= npages;
	vmstat.nreservedfree -= npages;
	vmstat.nactive += npages;
	update_page_use_stats(use, npages);

	memset((void *)vm_page_direct_map_addr(page), 0x0, PGSIZE * npages);

	*out = page;

	vmp_update_events();

	return 0;
}

int
vmp_page_alloc_locked(vm_page_t **out, enum vm_page_use use, bool must)
{
	return vmp_pages_alloc_locked(out, 0, use, must);
}

static vm_page_t *
page_buddy(vm_page_t *page)
{
	paddr_t paddr = vm_page_paddr(page) ^ ((1 << page->order) * PGSIZE);
#if BITS == 32
	if (paddr >= vmp_highest_address)
		return NULL;
#endif
	return vm_paddr_to_page(paddr );
}

static void
page_free(vm_page_t *page)
{
#if BITS == 64
	while (page->order < page->max_order) {
#else
	while (true) {
#endif
		vm_page_t *buddy = page_buddy(page);

#if BITS == 32
		if (buddy == NULL)
			break;
#endif

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

#if 0
		vm_page_t *buddy;
		intptr_t index = page - preg->pages, buddy_index;
		size_t pages = 1 << page->order;

		if (index % (2 * pages) == 0)
			buddy_index = index + pages;
		else
			buddy_index = index - pages;

		if ((size_t)buddy_index >= preg->npages || buddy_index < 0)
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
#endif
	}

	TAILQ_INSERT_HEAD(&buddy_queue[page->order], page, queue_link);
	buddy_queue_npages[page->order]++;
	page->on_freelist = true;

#if 1// def EXTREME_SANITY_CHECKING
	memset((void*)vm_page_direct_map_addr(page), 0x67, PGSIZE);
#endif
}

void
vmp_page_free_locked(vm_page_t *page)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->use == kPageUseDeleted);
	kassert(page->refcnt == 0);

#ifdef VM_TRACE_PAGE_ALLOC
	kprintf("Freeing page %p\n", page);
#endif

	page->dirty = false;
	page->referent_pte = 0;
	page->use = kPageUseFree;
	page->nonzero_ptes = 0;
	vmstat.nfree += npages;
	vmstat.nreservedfree += npages;
	vmstat.ndeleted -= npages;
	return page_free(page);
}

void
vmp_page_delete_locked(vm_page_t *page)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->use != kPageUseDeleted);
	kassert(!page->busy);

	update_page_use_stats(page->use, -npages);
	vmstat.ndeleted += npages;
	page->use = kPageUseDeleted;

	/* TODO: if page is anonymous, and drumslot != 0, then free drumslot */

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

void
vm_page_delete(vm_page_t *page)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	vmp_page_delete_locked(page);
	vmp_release_pfn_lock(ipl);
}

vm_page_t *
vmp_page_retain_locked(vm_page_t *page)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->refcnt >= 0);

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
			vmp_update_events();
		}
	}

	return page;
}

vm_page_t *
vm_page_retain(vm_page_t *page)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	vm_page_t *ret = vmp_page_retain_locked(page);
	vmp_release_pfn_lock(ipl);
	return ret;
}

void
vmp_page_release_locked(vm_page_t *page)
{
	size_t npages = vm_order_to_npages(page->order);

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->refcnt > 0);

	if (page->refcnt-- == 1) {
		/* going from active to inactive state */
		vmstat.nactive -= npages;

		switch (page->use) {
		case kPageUseDeleted:
			vmp_page_free_locked(page);
			return;

		case kPageUsePML1:
		case kPageUsePML2:
		case kPageUsePML3:
		case kPageUseAnonShared:
		case kPageUseAnonPrivate: {
			pte_t *thepte = (pte_t *)P2V(page->referent_pte);
			kassert(vmp_pte_characterise(thepte) == kPTEKindTrans);
			kassert(vmp_md_soft_pte_pfn(thepte) == page->pfn);
		}

		case kPageUseFileShared:
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
			vmp_update_events();
		} else {
			TAILQ_INSERT_TAIL(&vm_pagequeue_standby, page,
			    queue_link);
			vmstat.nstandby += npages;
		}
	}
}

void
vm_page_release(vm_page_t *page)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	vmp_page_release_locked(page);
	vmp_release_pfn_lock(ipl);
}

const char *
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
		return "anon_priv";
	case kPageUseAnonShared:
		return "anon_shar";
	case kPageUseFileShared:
		return "file";

	case kPageUsePML4:
		return "pml4";
	case kPageUsePML3:
		return "pml3";
	case kPageUsePML2:
		return "pml2";
	case kPageUsePML1:
		return "pml1";

	case kPageUseVPML4:
		return "prot_pml4";
	case kPageUseVPML3:
		return "prot_pml3";
	case kPageUseVPML2:
		return "prot_pml2";
	case kPageUseVPML1:
		return "prot_pml1";
	case kPageUseDeleted:
		return "deleted";
	default:
		return "BAD";
	}
}

static bool
page_is_pagetable(vm_page_t *page)
{
	return (page->use >= kPageUsePML1 && page->use <= kPageUsePML4) ||
	    (page->use >= kPageUseVPML1 && page->use <= kPageUseVPML4);
}


static void print_page_summary_header(void)
{
	kprintf("\033[7m"
		"%-8s%-11s%-6s%-6s%-10s%-6s%-6s"
		"\033[m\n",
	    "pfn", "use", "rc", "dirty", "off", "ptes", "nsptes");
}

static void
dump_page(vm_page_t *page)
{
	if (page->use == kPageUseFree || page->use == kPageUsePFNDB ||
	    page->use == kPageUseKWired)
		return;

	if (page_is_pagetable(page))
		kprintf("%-8zx%-11s%-6hu%-6s%-10s%-6hu%-6hu\n",
		    (size_t)page->pfn, vm_page_use_str(page->use), page->refcnt,
		    "n/a", "n/a", page->nonzero_ptes, page->noswap_ptes);
	else
		kprintf("%-8zx%-11s%-6hu%-6s%-10" PRIx64 "%-6s%-6s\n",
		    (size_t)page->pfn, vm_page_use_str(page->use), page->refcnt,
		    page->dirty ? "yes" : "no", (uint64_t)page->offset, "n/a",
		    "n/a");
}

void
vmp_pages_dump(void)
{
	kprintf("Active: %zu, modified: %zu, standby: %zu, free: %zu\n",
	    vmstat.nactive, vmstat.nmodified, vmstat.nstandby, vmstat.nfree);

	kprintf("\033[7m"
		"%-9s%-9s%-9s%-9s%-9s"
		"\033[m\n",
	    "free", "del", "priv", "fork", "file");
	kprintf("%-9zu%-9zu%-9zu%-9zu%-9zu\n", vmstat.nfree, vmstat.ndeleted,
	    vmstat.nanonprivate, vmstat.nanonfork, vmstat.nfileshared);

	kprintf("\033[7m"
		"%-9s%-9s%-9s%-9s%-9s"
		"\033[m\n",
	    "share", "pgtbl", "proto", "kwired", "pagedb");
	kprintf("%-9zu%-9zu%-9zu%-9zu%-9zu\n", vmstat.nanonshare,
	    vmstat.nprocpgtable, vmstat.nprotopgtable, vmstat.nkwired,
	    vmstat.npwired);

#if 0
	kprintf("Page summary:\n");
	print_page_summary_header();
	TAILQ_FOREACH (region, &pregion_queue, queue_entry) {
		for (int i = 0; i < region->npages; i++) {
			vm_page_t *page = &region->pages[i];
			dump_page(page);
		}
	}
#endif

	extern vm_procstate_t kernel_procstate;
	kprintf("Kernel working set:\n");
	vmp_wsl_dump(&kernel_procstate);
}

int
vm_page_alloc(vm_page_t **out, size_t order, enum vm_page_use use, bool must)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	int r = vmp_pages_alloc_locked(out, order, use, must);
	vmp_release_pfn_lock(ipl);
	return r;
}

void
vmp_page_steal(vm_page_t *page, enum vm_page_use new_use)
{
	pte_t *pte;
	vm_page_t *dirpage;

	kassert(ke_spinlock_held(&vmp_pfn_lock));
	kassert(page->refcnt == 0);
	kassert(!page->dirty);

	pte = (pte_t *)P2V(page->referent_pte);
	dirpage = vm_paddr_to_page(page->referent_pte);

	switch (page->use) {
	case kPageUsePML1:
	case kPageUsePML2:
	case kPageUsePML3: {
		vmstat.n_table_pageouts++;
		kassert(vmp_pte_characterise(pte) == kPTEKindTrans);
		vmp_md_swap_table_pointers(page->process->vm, dirpage, pte,
		    page->drumslot);
		break;
	}

	case kPageUseAnonShared:
	case kPageUseAnonPrivate: {
		kassert(vmp_pte_characterise(pte) == kPTEKindTrans);
		vmp_md_pte_create_swap(pte, page->drumslot);
		/* get ps from owner field */
		vmp_pagetable_page_pte_became_swap(page->process->vm, dirpage);
		break;
	}

	case kPageUseFileShared: {
		/* a file PTE is either valid, busy, or zero */
		kassert(vmp_pte_characterise(pte) == kPTEKindValid);
		vmp_md_pte_create_zero(pte);
		vmp_pagetable_page_pte_deleted(&kernel_procstate, dirpage,
		    false);
		break;
	}

	default:
		kfatal("Can't steal page of use %d\n", page->use);
	}

	/* TODO (low): this can be made more efficient */
	page->refcnt = 1;
	vmstat.nactive++;
	vmstat.nstandby--;
	vmp_page_delete_locked(page);
	vmp_page_release_locked(page);
}

void
vmp_modified_pages_dump(void)
{
	ipl_t ipl = vmp_acquire_pfn_lock();
	vm_page_t *page;

	kprintf("Modified Page Queue summary:\n");
	print_page_summary_header();
	TAILQ_FOREACH(page, &vm_pagequeue_modified, queue_link)
		dump_page(page);

	vmp_release_pfn_lock(ipl);
}
