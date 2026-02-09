/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/init.c
 * @brief Virtual memory initialisation.
 */

#include <keyronex/dlog.h>
#include <keyronex/ktypes.h>
#include <keyronex/limine.h>
#include <keyronex/vm.h>

#include <libkern/lib.h>

#include <stdbool.h>

#include "vm/page.h"

struct vm_phys_span {
	paddr_t base;
	size_t size;
	bool is_ram;
};

__attribute__((used, section(".requests")))
static volatile struct limine_executable_address_request kernel_address_req = {
	.id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
	.revision = 0
};

__attribute__((used, section(".requests")))
static volatile struct limine_memmap_request memmap_req = {
	.id = LIMINE_MEMMAP_REQUEST_ID,
	.revision = 0
};

static paddr_t bump_start, bump_l1_base, bump_l1_end, bump_small_base;
static paddr_t kpgtable;
static struct vm_phys_span *phys_spans;

#if !defined(__m68k__)
_Static_assert(sizeof(vm_page_t) == 64, "vm_page_t should be 64 bytes");
#else
_Static_assert(sizeof(vm_page_t) == 48, "vm_page_t should be 48 bytes");
#endif

paddr_t
boot_alloc(void)
{
	paddr_t ret = bump_small_base;
	bump_small_base += PGTABLE_SIZE;
	memset((void *)p2v(ret), 0, PGTABLE_SIZE);
	return ret;
}

#ifdef PGSIZE_L1
paddr_t
boot_alloc_l1(void)
{
	paddr_t ret = bump_l1_base;
	bump_l1_base += PGSIZE_L1;
	kassert(bump_l1_base <= bump_l1_end, "bump overrun");
	memset((void *)p2v(ret), 0, PGSIZE_L1);
	return ret;
}
#endif

static void
boot_map(uintptr_t vaddr, uintptr_t paddr, int level, vm_prot_t prot,
    vm_cache_mode_t cache)
{
	kdprintf("map vaddr %p to paddr %p\n", vaddr, paddr);
}

/*
 * Calculate how many large pages (L1) are needed to map the Resident Page Table
 * (RPT), set aside that many L1 pages for bump allocation, and then map the
 * RPT.
 */
static void
map_rpt(void)
{
	struct limine_memmap_entry **entries = memmap_req.response->entries;
	struct limine_memmap_entry *prev = NULL, *largest = NULL;
	size_t rpt_pages = 0;

	/* Page size for the resident page table. */
#ifdef PGSIZE_L1
	size_t rpt_pgsize = PGSIZE_L1;
	size_t rpt_level = 1;
#else
	size_t rpt_pgsize = PGSIZE;
	size_t rpt_level = 1;
#endif
	/* How many vm_page_t's in a largepage worth of RPT? */
	size_t rpt_page_elements_n = rpt_pgsize / sizeof(vm_page_t);
	/* How much memory is described by rpt_pgsize's worth of vm_page_t's? */
	size_t rpt_page_describes = rpt_page_elements_n * PGSIZE;

	for (size_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i];

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE)
			continue;

		uint64_t rounded_down_start = rounddown(entry->base,
		    rpt_page_describes);
		uint64_t rounded_up_end = roundup(entry->base + entry->length,
		    rpt_page_describes);
		uint64_t prev_rounded_up_end = prev ?
		    roundup(prev->base + prev->length, rpt_page_describes) :
		    0;

		if (prev && rounded_down_start < prev_rounded_up_end) {
			if (rounded_up_end > prev_rounded_up_end) {
				rpt_pages += (rounded_up_end -
				  prev_rounded_up_end) / rpt_page_describes;
			}
		} else {
			rpt_pages += (rounded_up_end - rounded_down_start) /
			    rpt_page_describes;
		}

		if (largest == NULL || entry->length > largest->length)
			largest = entry;

		prev = entry;
	}

	bump_start = roundup2(largest->base, rpt_pgsize);
	bump_l1_base = bump_start;
	bump_l1_end = bump_l1_base + rpt_pgsize * rpt_pages;
	bump_small_base = bump_l1_end;

	kpgtable = boot_alloc();

	prev = NULL;
	for (size_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i];

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE)
			continue;

		uint64_t rounded_down_start = rounddown(entry->base,
		    rpt_page_describes);
		uint64_t rounded_up_end = roundup(entry->base + entry->length,
		    rpt_page_describes);

		if (prev != NULL &&
		    rounded_down_start < roundup(prev->base + prev->length,
					     rpt_page_describes)) {
			rounded_down_start = roundup(prev->base + prev->length,
			    rpt_page_describes);
		}

		for (uint64_t base = rounded_down_start; base < rounded_up_end;
		     base += rpt_page_describes) {
			uintptr_t area = RPT_BASE +
			    (base / rpt_page_describes) * rpt_pgsize;
#ifdef PGSIZE_L1
			uintptr_t page = boot_alloc_l1();
#else
			uintptr_t page = boot_alloc();
#endif

			boot_map(area, page, 1, VM_READ | VM_WRITE,
			    kCacheModeDefault);
		}

		prev = entry;
	}
}


void
vm_phys_init(void)
{
	for (size_t j = 0; j < FREELIST_ORDERS; j++) {
		TAILQ_INIT(&vm_domain.free_q[j]);
		TAILQ_INIT(&vm_domain.stby_q);
		TAILQ_INIT(&vm_domain.dirty_q);
	}
	map_rpt();
#if 0
	map_hhdm();
	map_ksegs();
#if defined(__amd64__)
	map_lapic();
#endif
	pmap_set_kpgtable(kpgtable);
	add_phys_segs();
	unfree_boot();
	unfree_reserved();
	setup_permanent_tables();
#endif
}
