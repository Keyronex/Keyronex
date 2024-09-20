/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Wed Aug 21 2024.
 */
/*!
 * @file kernel/vm/init.c
 * @brief Basic initialisation of the VM system: setting up the kernel page
 * tables, etc.
 */

#include <kdk/libkern.h>
#include <kdk/vm.h>
#include <limine.h>
#include <stdint.h>

#include "kdk/kern.h"
#include "kdk/vmtypes.h"
#include "vm/vmp.h"

extern struct limine_memmap_request memmap_request;
extern struct limine_kernel_address_request kernel_address_request;
extern struct limine_framebuffer_request fb_request;

static paddr_t bump_start, bump_large_base, bump_large_end, bump_small_base;
static paddr_t kpgtable;

/* How big are PFNDB pages? */
static size_t pfndb_pgsize;
/* How many elements does each PFNDB page fit? */
static size_t elements_per_pfndb_page;
/* How many bytes of memory does a PFNDB page describe? */
static size_t pfndb_page_describes;

/* What level are HHDM mappings? */
static int hhdm_level;
/* How many bytes of memory does an HHDM page describe? */
static size_t hhdm_page_describes;

extern char TEXT_SEGMENT_START[];
extern char TEXT_SEGMENT_END[];
extern char RODATA_SEGMENT_START[];
extern char RODATA_SEGMENT_END[];
extern char DATA_SEGMENT_START[];
extern char KDATA_SEGMENT_END[];

_Static_assert(sizeof(vm_page_t) == sizeof(uintptr_t) * 8,
    "vm_page_t must be 8 words");

#ifdef PGSIZE_L2
paddr_t
boot_alloc_l2(void)
{
	paddr_t ret = bump_large_base;
	bump_large_base += PGSIZE_L2;
	kassert(bump_large_base <= bump_large_end);
	memset((void *)P2V(ret), 0, PGSIZE_L2);
	return ret;
}
#endif

paddr_t
boot_alloc(void)
{
	paddr_t ret = bump_small_base;
	bump_small_base += PGSIZE;
	memset((void *)P2V(ret), 0, PGSIZE);
	return ret;
}

#if defined(__amd64__)
static void
vmp_md_boot_setup_table_pointers(int level, pte_t *dirpte, paddr_t table)
{
	pte_t pte;
	pte.value = 0x0;
	pte.hw.valid = 1;
	pte.hw.writeable = 1;
	pte.hw.user = 1;
	pte.hw.pfn = table >> VMP_PAGE_SHIFT;
	dirpte->value = pte.value;
}
#elif defined(__aarch64__)
static void
vmp_md_boot_setup_table_pointers(int level, pte_t *dirpte, paddr_t table)
{
	pte_t pte;
	pte.value = 0x0;
	pte.hw.valid = 1;
	pte.hw_table.is_table = 1;
	pte.hw_table.next_level = table >> VMP_PAGE_SHIFT;
	dirpte->value = pte.value;
}

#elif defined(__riscv)
static void
vmp_md_boot_setup_table_pointers(int level, pte_t *dirpte, paddr_t table)
{
	pte_t pte;
	pte.value = 0x0;
	pte.hw.valid = 1;
	pte.hw.pfn = table >> VMP_PAGE_SHIFT;
	dirpte->value = pte.value;
}

#elif defined (__m68k__)
static void
vmp_md_boot_setup_table_pointers(int level, pte_t *dirpte, paddr_t table)
{
	size_t npages;

	kassert(level > 1);
	npages = level == 3 ? 8 : 16;

	dirpte = (pte_t *)ROUNDDOWN(dirpte, npages * sizeof(pte_t));

	for (int i = 0; i < npages; i++) {
		pte_t pte;
		pte.hw_pml2_040.addr = (table + i * PGSIZE / npages) >> 4;
		pte.hw_pml2_040.used = 0;
		pte.hw_pml2_040.writeprotect = 0;
		pte.hw_pml2_040.type = 3;
		dirpte[i].value = pte.value;
	}
}
#endif

int
boot_fetch_pte(int at_level, vaddr_t vaddr, pte_t **pte_out)
{
	int indexes[VMP_TABLE_LEVELS + 1];
	pte_t *table;

	vmp_addr_unpack(vaddr, indexes);

	table = (pte_t *)P2V(kpgtable);

	for (int level = VMP_TABLE_LEVELS; level > 0; level--) {
		pte_t *pte = &table[indexes[level]];

		/* note - level is 1-based */

		if (level == at_level) {
			*pte_out = pte;
			return 0;
		}

		if (vmp_pte_characterise(pte) == kPTEKindZero) {
			paddr_t new_table = boot_alloc();
			vmp_md_boot_setup_table_pointers(level, pte, new_table);
		} else if (vmp_pte_characterise(pte) != kPTEKindValid) {
			kfatal("unexpected PTE kind\n");
		}

		kassert(!vmp_md_pte_hw_is_large(pte, level));

		table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
	}
	kfatal("unreached\n");
}

static void
boot_map(uintptr_t vaddr, uintptr_t paddr, vm_protection_t prot, bool cacheable)
{
	pte_t *pte;
	boot_fetch_pte(1, vaddr, &pte);
	kassert(pte->value == 0);
	vmp_md_pte_create_hw(pte, paddr >> VMP_PAGE_SHIFT, prot & kVMWrite,
	    prot & kVMExecute, cacheable, false);
}

static void
boot_map_l2(uintptr_t vaddr, uintptr_t paddr, vm_protection_t prot,
    bool cacheable)
{
	pte_t *pte;

	boot_fetch_pte(2, vaddr, &pte);
	kassert(pte->value == 0);
	vmp_md_pte_create_hwl2(pte, paddr >> VMP_PAGE_SHIFT, prot & kVMWrite,
	    prot & kVMExecute, cacheable, false);
}

#if 0
static void
boot_map_l3(uintptr_t vaddr, uintptr_t paddr, vm_protection_t prot,
    bool cacheable)
{
	pte_t *pte;
	boot_fetch_pte(2, vaddr, &pte);
	vmp_md_pte_create_hwl3(pte, paddr >> VMP_PAGE_SHIFT, prot & kVMWrite,
	    prot & kVMExecute, cacheable, false);
}
#endif

static void
map_pfndb(void)
{
	struct limine_memmap_entry **entries = memmap_request.response->entries;
	struct limine_memmap_entry *prev = NULL, *largest = NULL;
	size_t rpt_pages = 0;

	for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i];

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE)
			continue;

		uint64_t roundedDownStart = ROUNDDOWN(entry->base,
		    pfndb_page_describes);
		uint64_t roundedUpEnd = ROUNDUP(entry->base + entry->length,
		    pfndb_page_describes);
		uint64_t prevRoundedUpEnd = prev ?
		    ROUNDUP(prev->base + prev->length, pfndb_page_describes) :
		    0;

		if (prev && roundedDownStart < prevRoundedUpEnd) {
			if (roundedUpEnd > prevRoundedUpEnd) {
				rpt_pages += (roundedUpEnd - prevRoundedUpEnd) /
				    pfndb_page_describes;
			}
		} else {
			rpt_pages += (roundedUpEnd - roundedDownStart) /
			    pfndb_page_describes;
		}

		if (largest == NULL || entry->length > largest->length)
			largest = entry;

		prev = entry;
	}

	bump_start = ROUNDUP(largest->base, pfndb_pgsize);
	bump_large_base = bump_start;
	bump_large_end = bump_large_base + pfndb_pgsize * rpt_pages;
	bump_small_base = bump_large_end;

	kpgtable = boot_alloc();

	prev = NULL;
	for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i];

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE)
			continue;

		uint64_t roundedDownStart = ROUNDDOWN(entry->base,
		    pfndb_page_describes);
		uint64_t roundedUpEnd = ROUNDUP(entry->base + entry->length,
		    pfndb_page_describes);

		if (prev != NULL &&
		    roundedDownStart < ROUNDUP(prev->base + prev->length,
					   pfndb_page_describes)) {
			roundedDownStart = ROUNDUP(prev->base + prev->length,
			    pfndb_page_describes);
		}

		for (uint64_t base = roundedDownStart; base < roundedUpEnd;
		     base += pfndb_page_describes) {
			uintptr_t area = PFNDB_BASE +
			    (base / pfndb_page_describes) * pfndb_pgsize;

#ifdef PGSIZE_L2
			uintptr_t page = boot_alloc_l2();
			boot_map_l2(area, page, kVMRead | kVMWrite, true);
#else
			uintptr_t page = boot_alloc();
			boot_map(area, page, kVMRead | kVMWrite, true);
#endif
		}

		prev = entry;
	}
}

static void
map_hhdm(void)
{
	struct limine_memmap_entry **entries = memmap_request.response->entries;
	struct limine_memmap_entry *prev = NULL;
	for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i];

		const char *type;

		switch (entry->type) {
		case LIMINE_MEMMAP_USABLE:
			type = "usable";
			break;

		case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
			type = "bootloader reclaimable";
			break;

		case LIMINE_MEMMAP_FRAMEBUFFER:
			type = "framebuffer";
			break;

		default:
			type = "unknown";
		}

		kprintf("Entry %zu: 0x%zx-0x%zx, type %s\n", i,
		    (uintptr_t)entry->base,
		    (uintptr_t)(entry->base + entry->length), type);

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE)
			continue;

		uint64_t roundedDownStart = ROUNDDOWN(entry->base,
		    hhdm_page_describes);
		uint64_t roundedUpEnd = ROUNDUP(entry->base + entry->length,
		    hhdm_page_describes);

		if (prev != NULL &&
		    roundedDownStart < ROUNDUP(prev->base + prev->length,
					   hhdm_page_describes)) {
			roundedDownStart = ROUNDUP(prev->base + prev->length,
			    hhdm_page_describes);
		}

		for (uint64_t base = roundedDownStart; base < roundedUpEnd;
		     base += hhdm_page_describes) {
			if (hhdm_level == 1)
				boot_map(HHDM_BASE + base, base,
				    kVMRead | kVMWrite, true);
			else
				boot_map_l2(HHDM_BASE + base, base,
				    kVMRead | kVMWrite, true);
		}

		prev = entry;
	}
}

static void
map_ksegs(void)
{
	uintptr_t start;
	uintptr_t limit;
	vaddr_t vbase = kernel_address_request.response->virtual_base;
	paddr_t pbase = kernel_address_request.response->physical_base;

	start = ROUNDDOWN((uintptr_t)TEXT_SEGMENT_START, PGSIZE);
	limit = ROUNDUP((uintptr_t)TEXT_SEGMENT_END, PGSIZE);
	for (uintptr_t vaddr = start; vaddr != limit; vaddr += PGSIZE) {
		boot_map(vaddr, vaddr - vbase + pbase, kVMRead | kVMExecute,
		    true);
	}

	start = ROUNDDOWN((uintptr_t)RODATA_SEGMENT_START, PGSIZE);
	limit = ROUNDUP((uintptr_t)RODATA_SEGMENT_END, PGSIZE);
	for (uintptr_t vaddr = start; vaddr != limit; vaddr += PGSIZE) {
		boot_map(vaddr, vaddr - vbase + pbase, kVMRead, true);
	}

	start = ROUNDDOWN((uintptr_t)DATA_SEGMENT_START, PGSIZE);
	limit = ROUNDUP((uintptr_t)KDATA_SEGMENT_END, PGSIZE);
	for (uintptr_t vaddr = start; vaddr != limit; vaddr += PGSIZE) {
		boot_map(vaddr, vaddr - vbase + pbase, kVMRead | kVMWrite,
		    true);
	}
}

static void
map_fbs(void)
{
#if !defined(__m68k__)
	struct limine_framebuffer *fb = fb_request.response->framebuffers[0];
	paddr_t fb0_phys = V2P(fb->address);
	paddr_t fb0_base = ROUNDDOWN(fb0_phys, hhdm_page_describes);
	paddr_t fb0_limit = ROUNDUP(fb0_phys + fb->pitch * fb->height,
	    hhdm_page_describes);

	for (paddr_t p = fb0_base; p < fb0_limit; p += hhdm_page_describes) {
		boot_map_l2(MISC_BASE + p, p, kVMRead | kVMWrite, true);
	}

	fb->address = (void *)(MISC_BASE + fb0_phys);
#endif
}

static void
add_phys_segs(void)
{
	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i], *next;
		paddr_t base, limit;

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
		    entry->type != LIMINE_MEMMAP_USABLE)
			continue;

		base = entry->base;
		limit = entry->base + entry->length;

		/* merge consecutive BOOTLOADER_RECLAIMABLE/USABLE regions */
		for (size_t j = i + 1; j < memmap_request.response->entry_count;
		     j++) {
			next = entries[j];

			if (next->type !=
				LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
			    next->type != LIMINE_MEMMAP_USABLE)
				break;

			if (entry->base + entry->length == next->base) {
				limit += next->length;
				i++;
			}
		}

		vm_region_add(base, limit);
	}
}

static void
unfree_boot(void)
{
	for (paddr_t i = bump_start; i < bump_large_end; i += pfndb_pgsize) {
		vm_page_t *page = vm_paddr_to_page(i);
		vmp_page_unfree(page, vm_bytes_to_order(pfndb_pgsize));
	}

	for (paddr_t i = bump_large_end; i < ROUNDUP(bump_small_base, PGSIZE);
	     i += PGSIZE) {
		vm_page_t *page = vm_paddr_to_page(i);
		vmp_page_unfree(page, 0);
	}
}

static void
unfree_bl_reserved(void)
{
	struct limine_memmap_entry **entries = memmap_request.response->entries;

	for (size_t i = 0; i < memmap_request.response->entry_count; i++) {
		struct limine_memmap_entry *entry = entries[i], *next;
		paddr_t base, limit;

		if (entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
			continue;

		base = entry->base;
		limit = entry->base + entry->length;

		for (size_t j = i + 1; j < memmap_request.response->entry_count;
		     j++) {
			next = entries[j];

			if (next->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
				break;

			if (entry->base + entry->length == next->base) {
				limit += next->length;
				i++;
			}
		}

		vmp_range_unfree(base, limit);
	}
}

void
vmp_pmm_init(void)
{
#ifdef PGSIZE_L2
	pfndb_pgsize = PGSIZE_L2;
#else
	pfndb_pgsize = PGSIZE;
#endif
	elements_per_pfndb_page = pfndb_pgsize / sizeof(vm_page_t);
	pfndb_page_describes = elements_per_pfndb_page * PGSIZE;

#ifdef PGSIZE_L2
	hhdm_level = 2;
	hhdm_page_describes = PGSIZE_L2;
#else
	hhdm_level = 1;
	hhdm_page_describes = PGSIZE;
#endif

	map_pfndb();
	map_hhdm();
	map_ksegs();
	map_fbs();
#if defined(__aarch64__)
	/* todo: get this out (UART) */
	boot_map(P2V(0x09000000), 0x09000000, kVMRead | kVMWrite, false);
	asm volatile("dsb ishst\n\t"
		     "msr ttbr1_el1, %0\n\t"
		     "tlbi vmalle1\n\t"
		     "dsb ish\n\t"
		     "isb\n\t"
		     :
		     : "r"(kpgtable)
		     : "memory");
#elif defined(__amd64__)
	asm volatile("mov %0, %%cr3" ::"r"(kpgtable) : "memory");
#elif defined(__riscv)
	asm volatile("csrw satp, %0\n\t"
		     "sfence.vma\n\t"
		     :
		     : "r"((kpgtable >> 12) | (0x9UL << 60))
		     : "memory");
#elif defined(__m68k__)
	extern volatile char *gftty_regs;
	for (paddr_t i = 0xff000000; i < 0xff020000; i += PGSIZE)
		boot_map(i, i, kVMRead | kVMWrite, false);
	asm volatile("movec %0, %%urp\n\t"
		     "movec %0, %%srp"
		     :
		     : "r"(kpgtable)
		     : "memory");
	gftty_regs = (void *)0xff008000;
#else
	for (;;)
		;
#endif

	add_phys_segs();
	unfree_boot();
	unfree_bl_reserved();

	kernel_procstate.md.table = kpgtable;

	// kfatal("Stop please\n");
}
