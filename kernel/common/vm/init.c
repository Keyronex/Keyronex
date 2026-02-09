/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/init.c
 * @brief Virtual memory initialisation.
 */

#include <keyronex/vm.h>
#include <keyronex/limine.h>

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
