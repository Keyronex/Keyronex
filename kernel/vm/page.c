/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */

#include "bsdqueue/queue.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vm.h"

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

#define DEFINE_PAGEQUEUE(NAME) \
	static page_queue_t NAME = TAILQ_HEAD_INITIALIZER(NAME)

typedef TAILQ_HEAD(, vm_page) page_queue_t;

struct vm_stat vmstat;
static TAILQ_HEAD(, vmp_pregion) pregion_queue = TAILQ_HEAD_INITIALIZER(
    pregion_queue);
kspinlock_t vmp_pfn_lock = KSPINLOCK_INITIALISER;
static page_queue_t free_list = TAILQ_HEAD_INITIALIZER(free_list);
DEFINE_PAGEQUEUE(vm_pagequeue_active);
DEFINE_PAGEQUEUE(vm_pagequeue_inactive);

void
vmp_region_add(paddr_t base, size_t length)
{
	struct vmp_pregion *bm = P2V(base);
	size_t used; /* n bytes used by bitmap struct */
	int b;

	/* set up a pregion for this area */
	bm->base = base;
	bm->npages = length / PGSIZE;

	used = ROUNDUP(sizeof(struct vmp_pregion) +
		sizeof(vm_page_t) * bm->npages,
	    PGSIZE);

	kdprintf("Usable memory area: 0x%lx "
		 "(%lu MiB, %lu pages)\n",
	    base, length / (1024 * 1024), length / PGSIZE);
	kdprintf("%lu KiB for PFN database part\n", used / 1024);

	/* initialise pages */
	for (b = 0; b < bm->npages; b++) {
		bm->pages[b].pfn = (bm->base + PGSIZE * b) >> 12;
		bm->pages[b].anon = NULL;
		LIST_INIT(&bm->pages[b].pv_list);
	}

	/* mark off the pages used */
	for (b = 0; b < used / PGSIZE; b++) {
		bm->pages[b].use = kPageUseVMM;
		bm->pages[b].wirecnt = 1;
	}

	/* now zero the remainder */
	for (; b < bm->npages; b++) {
		bm->pages[b].use = kPageUseFree;
		bm->pages[b].wirecnt = 0;
		TAILQ_INSERT_TAIL(&free_list, &bm->pages[b], queue_entry);
	}

	vmstat.nvmm += used / PGSIZE;
	vmstat.nfree += bm->npages - (used / PGSIZE);

	TAILQ_INSERT_TAIL(&pregion_queue, bm, queue_entry);
}

vm_page_t *
vmp_paddr_to_page(paddr_t paddr)
{
	struct vmp_pregion *preg;

	TAILQ_FOREACH (preg, &pregion_queue, queue_entry) {
		if (preg->base <= paddr &&
		    (preg->base + PGSIZE * preg->npages) > paddr) {
			return &preg->pages[(paddr - preg->base) / PGSIZE];
		}
	}

	return NULL;
}

int
vmp_page_alloc(vm_map_t *ps, bool must, enum vm_page_use use, vm_page_t **out)
{
	vm_page_t *page = TAILQ_FIRST(&free_list);
	kassert(page);
	TAILQ_REMOVE(&free_list, page, queue_entry);

	kassert(page->wirecnt == 0);
	vmstat.nfree--;

	page->use = use;
	page->wirecnt = 1;

	*out = page;

	memset(VM_PAGE_DIRECT_MAP_ADDR(page), 0x0, PGSIZE);

	return 0;
}

void
vmp_page_free(vm_map_t *ps, vm_page_t *page)
{
	TAILQ_INSERT_HEAD(&free_list, page, queue_entry);
	vmstat.nfree++;
}

void
vmp_page_copy(vm_page_t *from, vm_page_t *to)
{
	memcpy(VM_PAGE_DIRECT_MAP_ADDR(to), VM_PAGE_DIRECT_MAP_ADDR(from),
	    PGSIZE);
}

paddr_t
vm_translate(vaddr_t vaddr)
{
	if (vaddr >= HHDM_BASE && vaddr < HHDM_BASE + HHDM_SIZE) {
		return (paddr_t)V2P(vaddr);
	} else {
		paddr_t r;
		kassert(vaddr > HHDM_BASE);
		r = pmap_trans(&kernel_process.map, vaddr);
		kassert(r != 0);
		return r;
	}
}

#define MDL_SIZE(NPAGES) (sizeof(vm_mdl_t) + sizeof(vm_page_t *) * NPAGES)

vm_mdl_t *
vm_mdl_alloc(size_t npages)
{
	vm_mdl_t *mdl = kmem_alloc(MDL_SIZE(npages));
	mdl->npages = npages;
	return mdl;
}

vm_mdl_t *
vm_mdl_buffer_alloc(size_t npages)
{
	vm_mdl_t *mdl = kmem_alloc(MDL_SIZE(npages));
	mdl->npages = npages;
	for (unsigned i = 0; i < npages; i++) {
		int r = vmp_page_alloc(&kernel_process.map, true, kPageUseWired,
		    &mdl->pages[i]);
		kassert(r == 0);
	}
	return mdl;
}

void
vm_mdl_map(vm_mdl_t *mdl, void **out)
{
	kassert(mdl->npages == 1);
	*out = VM_PAGE_DIRECT_MAP_ADDR(mdl->pages[0]);
}

void
vm_mdl_memcpy(void *dest, vm_mdl_t *mdl, voff_t off, size_t n)
{
	voff_t base = PGROUNDDOWN(off);
	voff_t pageoff = off - base;
	size_t firstpage = base / PGSIZE;
	size_t lastpage = firstpage + (pageoff + n - 1) / PGSIZE + 1;

	for (size_t iPage = firstpage; iPage < lastpage; iPage++) {
		vm_page_t *page;
		size_t tocopy;

		if (n > PGSIZE)
			tocopy = PGSIZE - pageoff;
		else
			tocopy = n;

		page = mdl->pages[iPage];

		memcpy(dest + (iPage - firstpage) * PGSIZE,
		    VM_PAGE_DIRECT_MAP_ADDR(page) + pageoff, tocopy);

		n -= tocopy;
		pageoff = 0;
	}
}

paddr_t
vm_mdl_paddr(vm_mdl_t *mdl, voff_t offset)
{
	kassert(offset % PGSIZE == 0);
	return VM_PAGE_PADDR(mdl->pages[offset / PGSIZE]);
}

void
vm_page_wire(vm_page_t *page)
{
	ipl_t ipl = vmp_acquire_pfn_lock();

	switch (page->status) {
	case kPageStatusActive:
		TAILQ_REMOVE(&vm_pagequeue_active, page, queue_entry);
		vmstat.nactive--;
		vmstat.nwired++;
		break;

	case kPageStatusInactive:
		TAILQ_REMOVE(&vm_pagequeue_active, page, queue_entry);
		vmstat.ninactive--;
		vmstat.nwired++;
		break;

	case kPageStatusWired:
	    /* epsilon */
	    ;

	case kPageStatusBusy:
		kfatal("Cannot wire a busy page.\n");
	}

	page->status = kPageStatusWired;

	vmp_release_pfn_lock(ipl);
}

void
vm_page_unwire(vm_page_t *page)
{
	ipl_t ipl = vmp_acquire_pfn_lock();

	kassert(page->status == kPageStatusWired);
	kassert(page->wirecnt > 0);

	if (--page->wirecnt == 0) {
		if (page->use == kPageUseAnonymous ||
		    page->use == kPageUseObject) {
			TAILQ_INSERT_HEAD(&vm_pagequeue_active, page,
			    queue_entry);
			vmstat.nwired--;
			vmstat.nactive++;
		}
	}

	vmp_release_pfn_lock(ipl);
}