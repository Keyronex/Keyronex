/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Mon Feb 13 2023.
 */

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

struct vm_stat vmstat;
static TAILQ_HEAD(, mi_pregion) pregion_queue = TAILQ_HEAD_INITIALIZER(
    pregion_queue);

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
	}

	vmstat.npfndb += used / PGSIZE;
	vmstat.nfree += bm->npages - (used / PGSIZE);

	TAILQ_INSERT_TAIL(&pregion_queue, bm, queue_entry);
}