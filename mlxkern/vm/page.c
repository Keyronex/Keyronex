/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */

#include "bsdqueue/queue.h"
#include "libkern/libkern.h"
#include "vm/amd64/vm_md.h"
#include "vm/vm.h"

struct mi_pregion {
	/*! Linkage to pregion_queue. */
	TAILQ_ENTRY(mi_pregion) queue_entry;
	/*! Base address of region. */
	paddr_t base;
	/*! Number of pages the region covers. */
	size_t npages;
	/*! PFN database part for region. */
	vm_page_t pages[0];
};

typedef STAILQ_HEAD(, vm_page) page_queue_t;

struct vm_stat vmstat;
static TAILQ_HEAD(, mi_pregion) pregion_queue = TAILQ_HEAD_INITIALIZER(
    pregion_queue);
kspinlock_t vi_pfn_lock = KSPINLOCK_INITIALISER;
static page_queue_t free_list = STAILQ_HEAD_INITIALIZER(free_list);

void
vi_region_add(paddr_t base, size_t length)
{
	struct mi_pregion *bm = P2V(base);
	size_t used; /* n bytes used by bitmap struct */
	int b;

	/* set up a pregion for this area */
	bm->base = base;
	bm->npages = length / PGSIZE;

	used = ROUNDUP(sizeof(struct mi_pregion) +
		sizeof(vm_page_t) * bm->npages,
	    PGSIZE);

	kdprintf("Usable memory area: 0x%lx "
		 "(%lu MiB, %lu pages)\n",
	    base, length / (1024 * 1024), length / PGSIZE);
	kdprintf("%lu KiB for PFN database part\n", used / 1024);

	/* initialise pages */
	for (b = 0; b < bm->npages; b++) {
		bm->pages[b].address = bm->base + PGSIZE * b;
		bm->pages[b].file = NULL;
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
vi_paddr_to_page(paddr_t paddr)
{
	struct mi_pregion *preg;

	TAILQ_FOREACH (preg, &pregion_queue, queue_entry) {
		if (preg->base <= paddr &&
		    (preg->base + PGSIZE * preg->npages) > paddr) {
			return &preg->pages[(paddr - preg->base) / PGSIZE];
		}
	}

	return NULL;
}

int
vi_page_alloc(vm_procstate_t *ps, bool must, enum vm_page_use use,
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

	return 0;
}

void
vi_page_free(vm_procstate_t *ps, vm_page_t *page)
{

	STAILQ_INSERT_HEAD(&free_list, page, queue_entry);
	vmstat.nfree++;
}

void
vmp_page_copy(vm_page_t *from, vm_page_t *to)
{
	memcpy(P2V(to->address), P2V(from->address), PGSIZE);
}