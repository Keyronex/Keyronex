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
#include <keyronex/pmap.h>
#include <keyronex/proc.h>
#include <keyronex/vm.h>

#include <libkern/lib.h>

#include <stdbool.h>

#include "vm/map.h"
#include "vm/page.h"

struct vm_phys_span {
	paddr_t base;
	size_t size;
	bool is_ram;
};

/* phys.c */
void vmp_region_add(paddr_t base, paddr_t limit);
void vmp_page_unfree(vm_page_t *page, size_t order);
void vmp_range_unfree(paddr_t base, paddr_t limit);

extern char TEXT_SEGMENT_START[];
extern char TEXT_SEGMENT_END[];
extern char RODATA_SEGMENT_START[];
extern char RODATA_SEGMENT_END[];
extern char DATA_SEGMENT_START[];
extern char KDATA_SEGMENT_END[];

extern struct vm_rs vm_kwired_rs;

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

static paddr_t bump_start, bump_rpt_base, bump_rpt_end, bump_small_base;
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
	bump_small_base += PGSIZE;
	memset((void *)p2v(ret), 0, PGSIZE);
	return ret;
}

paddr_t
boot_alloc_rpt(size_t size)
{
	paddr_t ret = bump_rpt_base;
	bump_rpt_base += size;
	kassert(bump_rpt_base <= bump_rpt_end, "bump overrun");
	memset((void *)p2v(ret), 0, size);
	return ret;
}

int
boot_fetch_pte(pte_t **pte_out, vaddr_t vaddr, pmap_level_t at_level)
{
	size_t indexes[PMAP_MAX_LEVELS];
	pte_t *table;

	pmap_indexes(vaddr, indexes);

	table = (pte_t *)p2v(kpgtable);

	for (pmap_level_t level = PMAP_LEVELS - 1;; level--) {
		pte_t *ppte = &table[indexes[level]];
		pte_t pte;
		enum pmap_pte_kind kind;

		if (level == at_level) {
			*pte_out = ppte;
			return 0;
		}

		pte = pmap_load_pte(ppte);
		kind = pmap_pte_characterise(pte);
		if (kind == kPTEKindZero) {
			paddr_t new_table = boot_alloc();
			pte = pmap_pte_hwdir_create(ppte, new_table, level);
		} else if (kind != kPTEKindHW) {
			kfatal("unexpected PTE kind\n");
		} else {
			kassert(!pmap_pte_hw_is_large(pte, level),
			    "boot_fetch_pte: encountered large");
		}

		table = (pte_t *)p2v(pmap_pte_hwdir_paddr(pte, level));
	}
	kfatal("unreached\n");
}

static void
boot_map(uintptr_t vaddr, uintptr_t paddr, pmap_level_t level, vm_prot_t prot,
    vm_cache_mode_t cache)
{
	pte_t *ppte, pte;
	boot_fetch_pte(&ppte, vaddr, level);
	pte = pmap_load_pte(ppte);
	kassert(pmap_pte_characterise(pte) == kPTEKindZero, "boot_map: not zero");
	pmap_pte_hwleaf_create(ppte, paddr >> PGSHIFT, level, prot, cache);
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
#if PMAP_L1_PAGES
	const size_t rpt_pgsize = PGSIZE_L1;
	const pmap_level_t rpt_level = PMAP_L1;
#else
	const size_t rpt_pgsize = PGSIZE;
	const pmap_level_t rpt_level = PMAP_L0;
#endif
	/* How many vm_page_t's in a largepage worth of RPT? */
	const size_t rpt_page_elements_n = rpt_pgsize / sizeof(vm_page_t);
	/* How much memory is described by rpt_pgsize's worth of vm_page_t's? */
	const size_t rpt_page_describes = rpt_page_elements_n * PGSIZE;

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
	bump_rpt_base = bump_start;
	bump_rpt_end = bump_rpt_base + rpt_pgsize * rpt_pages;
	bump_small_base = bump_rpt_end;

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
			uintptr_t page = boot_alloc_rpt(rpt_pgsize);

			boot_map(area, page, rpt_level, VM_READ | VM_WRITE,
			    kCacheModeDefault);
		}

		prev = entry;
	}
}

/*
 * Map the Higher Half Direct Map (HHDM) with as large pages as possible.
 */
void
map_hhdm()
{
	struct limine_memmap_entry **entries = memmap_req.response->entries;
	struct limine_memmap_entry *prev = NULL;

#if PMAP_L3_PAGES
	size_t hhdm_level = PMAP_L3;
	size_t hhdm_pgsize = PGSIZE_L3;
#elif PMAP_L2_PAGES
	size_t hhdm_level = PMAP_L2;
	size_t hhdm_pgsize = PGSIZE_L2;
#elif PMAP_L1_PAGES
	size_t hhdm_level = PMAP_L1;
	size_t hhdm_pgsize = PGSIZE_L1;
#else
	size_t hhdm_level = PMAP_L0;
	size_t hhdm_pgsize = PGSIZE;
#endif

	for (size_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i];
		uint64_t rounded_down_start, rounded_up_end;

		if (entry->type == LIMINE_MEMMAP_RESERVED ||
		    entry->type == LIMINE_MEMMAP_BAD_MEMORY)
			continue;

		rounded_down_start = rounddown2(entry->base, hhdm_pgsize);
		rounded_up_end = roundup2(entry->base + entry->length,
		    hhdm_pgsize);

		if (prev != NULL &&
		    rounded_down_start <
			roundup2(prev->base + prev->length, hhdm_pgsize))
			rounded_down_start = roundup2(prev->base + prev->length,
			    hhdm_pgsize);

		for (uint64_t base = rounded_down_start; base < rounded_up_end;
		    base += hhdm_pgsize) {
			boot_map(HHDM_BASE + base, base, hhdm_level,
			    VM_READ | VM_WRITE, kCacheModeDefault);
		}

		prev = entry;
	}
}

/*
 * Map the kernel segments.
 */
static void
map_ksegs(void)
{
	uintptr_t start;
	uintptr_t limit;
	vaddr_t vbase = kernel_address_req.response->virtual_base;
	paddr_t pbase = kernel_address_req.response->physical_base;

	start = rounddown2((uintptr_t)TEXT_SEGMENT_START, PGSIZE);
	limit = roundup2((uintptr_t)TEXT_SEGMENT_END, PGSIZE);
	for (uintptr_t vaddr = start; vaddr != limit; vaddr += PGSIZE)
		boot_map(vaddr, vaddr - vbase + pbase, PMAP_L0,
		    VM_READ | VM_EXEC, kCacheModeDefault);

	start = rounddown2((uintptr_t)RODATA_SEGMENT_START, PGSIZE);
	limit = roundup2((uintptr_t)RODATA_SEGMENT_END, PGSIZE);
	for (uintptr_t vaddr = start; vaddr != limit; vaddr += PGSIZE)
		boot_map(vaddr, vaddr - vbase + pbase, PMAP_L0, VM_READ,
		    kCacheModeDefault);

	start = rounddown2((uintptr_t)DATA_SEGMENT_START, PGSIZE);
	limit = roundup2((uintptr_t)KDATA_SEGMENT_END, PGSIZE);
	for (uintptr_t vaddr = start; vaddr != limit; vaddr += PGSIZE)
		boot_map(vaddr, vaddr - vbase + pbase, PMAP_L0,
		    VM_READ | VM_WRITE, kCacheModeDefault);
}

#if defined(__amd64__)
static void
map_arch(void)
{
	uint32_t eax, edx;
	uint64_t lapic_paddr;

	__asm__ volatile("rdmsr"
			 : "=a"(eax), "=d"(edx)
			 : "c"(0x1B)); /* IA32_APIC_BASE MSR */

	lapic_paddr = ((uint64_t)edx << 32) | eax;
	lapic_paddr &= ~((1 << 12) - 1);

	boot_map(HHDM_BASE + lapic_paddr, lapic_paddr, PMAP_L0, VM_READ | VM_WRITE,
	    kCacheModeUC);
}
#elif defined (__m68k__)
static void
map_arch(void)
{
	for (paddr_t i = 0xff000000; i < 0xff020000; i += PGSIZE)
		boot_map(i, i, PMAP_L0, VM_READ | VM_WRITE, kCacheModeUC);
}
#endif

void
pmap_set_kpgtable(void)
{
#if defined(__amd64__)
	asm volatile("mov %0, %%cr3" ::"r"(kpgtable) : "memory");
#elif defined(__m68k__)
	extern volatile char *gftty_regs;
	asm volatile("movec %0, %%urp\n\t"
		     "movec %0, %%srp"
		     :
		     : "r"(kpgtable)
		     : "memory");
	gftty_regs = (void *)0xff008000;
#else
	kfatal("port me");
#endif
}


static void
add_phys_segs(void)
{
	struct limine_memmap_entry **entries = memmap_req.response->entries;

	for (size_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i], *next;
		paddr_t base, limit;

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE &&
		    entry->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
			continue;

		base = entry->base;
		limit = entry->base + entry->length;

		/* merge consecutive compatible regions */
		for (size_t j = i + 1; j < memmap_req.response->entry_count;
		     j++) {
			next = entries[j];

			if (next->type !=
				LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
			    next->type != LIMINE_MEMMAP_USABLE &&
			    next->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
				break;

			if (limit == next->base) {
				limit += next->length;
				i++;
			}
		}

		vmp_region_add(base, limit);
	}
}

static void
unfree_boot(void)
{
#if PMAP_L1_PAGES
	for (paddr_t i = bump_start; i < bump_rpt_end; i += PGSIZE_L1) {
		vm_page_t *page = VM_PAGE_FOR_PADDR(i);
		vmp_page_unfree(page, vm_bytes_to_order(PGSIZE_L1));
	}
#else
	for (paddr_t i = bump_start; i < bump_rpt_end; i += PGSIZE) {
		vm_page_t *page = VM_PAGE_FOR_PADDR(i);
		vmp_page_unfree(page, 0);
	}
#endif

	for (paddr_t i = bump_rpt_end; i < roundup2(bump_small_base, PGSIZE);
	    i += PGSIZE) {
		vm_page_t *page = VM_PAGE_FOR_PADDR(i);
		vmp_page_unfree(page, 0);
	}
}

static void
unfree_reserved(void)
{
	struct limine_memmap_entry **entries = memmap_req.response->entries;

	for (size_t i = 0; i < memmap_req.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i], *next;
		paddr_t base, limit;

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
			continue;

		base = entry->base;
		limit = entry->base + entry->length;

		for (size_t j = i + 1; j < memmap_req.response->entry_count;
		     j++) {
			next = entries[j];

			if (next->type !=
				LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
			    next->type != LIMINE_MEMMAP_EXECUTABLE_AND_MODULES)
				break;

			if (entry->base + entry->length == next->base) {
				limit += next->length;
				i++;
			}
		}

		vmp_range_unfree(base, limit);
	}
}


static void
setup_permanent_tables(void)
{
	vm_page_t *root_page = VM_PAGE_FOR_PADDR(kpgtable);

	root_page->ref_count++;
	root_page->use = VM_PAGE_TABLE;
	root_page->proctable.nonzero_ptes = 5001;
	root_page->proctable.noswap_ptes = 5001;
	root_page->proctable.valid_pageable_leaf_ptes = 0;
	root_page->proctable.level = PMAP_MAX_LEVELS - 1;
	root_page->proctable.is_root = true;
	root_page->owner_rs = &vm_kwired_rs;

	vm_kwired_rs.map = &kernel_map;

	for (vaddr_t vaddr = PIN_HEAP_BASE;
	     vaddr < PIN_HEAP_BASE + PIN_HEAP_SIZE; vaddr += PMAP_ROOTLEVEL_SPAN) {
		pte_t *pte;
		vm_page_t *page;

		boot_fetch_pte(&pte, vaddr, PMAP_MAX_LEVELS - 1);
		kassert(pmap_pte_characterise(pmap_load_pte(pte)) == kPTEKindZero);

		page = vm_page_alloc(VM_PAGE_TABLE, 0, VM_DOMID_LOCAL,
		    VM_NOFAIL);
		memset((void *)vm_page_hhdm_addr(page), 0, PGSIZE);

		page->proctable.level = PMAP_MAX_LEVELS - 2;
		page->proctable.nonzero_ptes = 1;
		page->proctable.noswap_ptes = 1;
		page->proctable.valid_pageable_leaf_ptes = 0;
		page->proctable.is_root = false;
		page->owner_rs = &vm_kwired_rs;
		pmap_pte_hwdir_create(pte, VM_PAGE_PADDR(page), PMAP_MAX_LEVELS - 1);
	}


	for (vaddr_t vaddr = PAGE_HEAP_BASE;
	     vaddr < MISC_MAP_BASE + MISC_MAP_SIZE; vaddr += PMAP_ROOTLEVEL_SPAN) {
		pte_t *pte;
		vm_page_t *page;

		boot_fetch_pte(&pte, vaddr, PMAP_MAX_LEVELS - 1);
		kassert(pmap_pte_characterise(pmap_load_pte(pte)) == kPTEKindZero);

		page = vm_page_alloc(VM_PAGE_TABLE, 0, VM_DOMID_LOCAL,
		    VM_NOFAIL);
		memset((void *)vm_page_hhdm_addr(page), 0, PGSIZE);

		page->proctable.level = PMAP_MAX_LEVELS - 2;
		page->proctable.nonzero_ptes = 1;
		page->proctable.noswap_ptes = 1;
		page->proctable.valid_pageable_leaf_ptes = 0;
		page->proctable.is_root = false;
		page->owner_rs = &proc0.vm_map->rs;
		pmap_pte_hwdir_create(pte, VM_PAGE_PADDR(page), PMAP_MAX_LEVELS - 1);
	}
}


void
vm_phys_init(void)
{
	for (size_t j = 0; j < FREELIST_ORDERS; j++) {
		TAILQ_INIT(&vm_domains[0].free_q[j]);
		TAILQ_INIT(&vm_domains[0].stby_q);
		TAILQ_INIT(&vm_domains[0].dirty_q);
	}

	map_rpt();
	map_hhdm();
	map_ksegs();
	map_arch();
	pmap_set_kpgtable();
	add_phys_segs();
	unfree_boot();
	unfree_reserved();
	setup_permanent_tables();
}
