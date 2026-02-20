/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file pmap.c
 * @brief Physical mapping for amd64
 */

#include <sys/pmap.h>
#include <sys/proc.h>
#include <sys/vm.h>

#include <libkern/lib.h>
#include <vm/map.h>

void
pmap_activate(vm_map_t *map)
{
	asm volatile("mov %0, %%cr3" ::"r"(map->pgtable) : "memory");
}

paddr_t
pmap_allocate_pgtable(vm_map_t *map)
{
	vm_page_t *page = vm_page_alloc(VM_PAGE_TABLE, 0, VM_DOMID_ANY,
	    VM_SLEEP);
	pte_t *pte = (pte_t *)vm_page_hhdm_addr(page);
	pte_t *kpte = (pte_t *)p2v(proc0.vm_map->pgtable);

	memset(pte, 0, 256 * sizeof(pte_t));
	memcpy(pte + 256, kpte + 256, 256 * sizeof(pte_t));

	/*
	 * set to 0x1000 as a sentinel;
	 * a pmap can be freed if nonzero_ptes == 0x1000
	 */
	page->proctable.nonzero_ptes = 0x1000;
	page->proctable.noswap_ptes = 0;
	page->proctable.valid_pageable_leaf_ptes = 0;
	page->proctable.level = PMAP_MAX_LEVELS - 1;
	page->proctable.is_root = true;
	page->owner_rs = &map->rs;

	return vm_page_paddr(page);
}
