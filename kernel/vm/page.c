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

typedef STAILQ_HEAD(, vm_page) page_queue_t;

struct vm_stat vmstat;
static TAILQ_HEAD(, vmp_pregion) pregion_queue = TAILQ_HEAD_INITIALIZER(
    pregion_queue);
kspinlock_t vmp_pfn_lock = KSPINLOCK_INITIALISER;
static page_queue_t free_list = STAILQ_HEAD_INITIALIZER(free_list);

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
		bm->pages[b].address = bm->base + PGSIZE * b;
		bm->pages[b].vnode = NULL;
	}

	/* mark off the pages used */
	for (b = 0; b < used / PGSIZE; b++) {
		bm->pages[b].use = kPageUseVMM;
	}

	/* now zero the remainder */
	for (; b < bm->npages; b++) {
		bm->pages[b].use = kPageUseFree;
		STAILQ_INSERT_TAIL(&free_list, &bm->pages[b], queue_entry);
	}

	vmstat.npfndb += used / PGSIZE;
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
vmp_page_alloc(vm_procstate_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out)
{
	vm_page_t *page = STAILQ_FIRST(&free_list);
	kassert(page);
	STAILQ_REMOVE_HEAD(&free_list, queue_entry);

	kassert(page->reference_count == 0);
	vmstat.nfree--;

	page->use = use;
	page->reference_count = 1;

	*out = page;

	memset(P2V(page->address), 0x0, PGSIZE);

	return 0;
}

void
vmp_page_free(vm_procstate_t *ps, vm_page_t *page)
{
	STAILQ_INSERT_HEAD(&free_list, page, queue_entry);
	vmstat.nfree++;
}

void
vmp_page_copy(vm_page_t *from, vm_page_t *to)
{
	memcpy(P2V(to->address), P2V(from->address), PGSIZE);
}

paddr_t
vm_translate(vaddr_t vaddr)
{
	if (vaddr >= HHDM_BASE && vaddr < HHDM_BASE + HHDM_SIZE) {
		return (paddr_t)V2P(vaddr);
	} else {
		paddr_t r;
		kassert(vaddr > HHDM_BASE);
		r = pmap_trans(&kernel_process.vmps, vaddr);
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
		int r = vmp_page_alloc(&kernel_process.vmps, true,
		    kPageUseWired, &mdl->pages[i]);
		kassert(r == 0);
	}
	return mdl;
}

void
vm_mdl_map(vm_mdl_t *mdl, void **out)
{
	kassert(mdl->npages == 1);
	*out = (void *)P2V(mdl->pages[0]->address);
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
		    P2V(page->address) + pageoff, tocopy);

		n -= tocopy;
		pageoff = 0;
	}
}