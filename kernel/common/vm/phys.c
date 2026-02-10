/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024 Cloudarox Solutions.
 */
/*!
 * @file phys.c
 * @brief Physical memory management.
 */

#include <keyronex/dlog.h>
#include <keyronex/ktypes.h>

#include "vm/page.h"

struct vm_affinity {
	paddr_t base;
	paddr_t limit;
	uint8_t domain;
};

vm_page_t *vm_pages = (vm_page_t *)RPT_BASE;

vm_domain_t vm_domains[1];
size_t highest_domid = 0;

struct vm_affinity affinities[1] = { { 0, UINTPTR_MAX, 0 } };
size_t naffinities = 1;

uint32_t
log2(uint32_t val)
{
	if (val == 0)
		return 0;
	return 32 - __builtin_clz(val - 1);
}

size_t
vm_npages_to_order(size_t npages)
{
	size_t order = log2(npages);
	if ((1 << order) < npages)
		order++;
	return order;
}

size_t
vm_bytes_to_order(size_t bytes)
{
	return vm_npages_to_order(roundup2(bytes, PGSIZE) / PGSIZE);
}

vaddr_t
vm_page_hhdm_addr(vm_page_t *page)
{
	return p2v((page - vm_pages) << PGSHIFT);
}

paddr_t
vm_page_paddr(vm_page_t *page)
{
	return (page - vm_pages) << PGSHIFT;
}

void
vmp_region_add(paddr_t base, paddr_t limit)
{
	if (base == limit)
		return;

	for (size_t aff_i = 0; aff_i < naffinities; aff_i++) {
		struct vm_affinity *aff = &affinities[aff_i];
		struct vm_domain *dom = &vm_domains[aff->domain];
		paddr_t region_base, region_limit;

		if (limit <= aff->base || base >= aff->limit)
			continue;

		if (aff->domain > highest_domid)
			highest_domid = aff->domain;

		region_base = MAX2(base, aff->base);
		region_limit = MIN2(limit, aff->limit);

		kdprintf("vm_region_add: 0x%zx-0x%zx (%zu kib; domid %d)\n",
		    region_base, region_limit,
		    (region_limit - region_base) / 1024, aff->domain);

		for (paddr_t i = region_base; i < region_limit; i += PGSIZE) {
			pfn_t pfn = i >> PGSHIFT;
			vm_page_t *page = &vm_pages[pfn];
			size_t order = MIN2(FREELIST_ORDERS - 1,
			    __builtin_ctz(pfn));

			while ((i + (1 << order) * PGSIZE) > region_limit)
				order--;

			page->domain = aff->domain;
			page->order = order;
			page->on_freelist = false;
		}

		for (paddr_t i = region_base; i < region_limit;) {
			vm_page_t *page = &vm_pages[i >> PGSHIFT];
			TAILQ_INSERT_HEAD(&dom->free_q[page->order], page,
			    qlink);
			dom->free_n[page->order]++;
			i += (1 << page->order) * PGSIZE;
			page->on_freelist = true;
			page->max_order = page->order;
		}

		dom->use_n[VM_PAGE_FREE] += (region_limit - region_base) /
		    PGSIZE;
	}
}

void
vmp_page_unfree(vm_page_t *page, size_t order)
{
	vm_page_t *initial = page;
	struct vm_domain *dom = &vm_domains[page->domain];

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
	kassert_dbg(initial->order >= order, "bad order in vm_page_unfree");
	while (initial->order != order) {
		vm_page_t *second = initial + (1 << (initial->order - 1));
		size_t new_order;

		new_order = second->order;
		kassert_dbg(new_order == initial->order - 1, "vm_page_unfree");
		kassert_dbg(initial->on_freelist, "vm_page_unfree");
		kassert_dbg(!second->on_freelist, "vm_page_unfree");

		dom->free_n[initial->order]--;
		dom->free_n[new_order] += 2;

		second->on_freelist = true;
		TAILQ_INSERT_HEAD(&dom->free_q[new_order], second, qlink);

		TAILQ_REMOVE(&dom->free_q[initial->order], initial, qlink);

		initial->order = new_order;
		TAILQ_INSERT_HEAD(&dom->free_q[new_order], initial, qlink);

		if (second <= page)
			initial = second;
	}

	kassert_dbg(initial == page, "vm_page_unfree");
	kassert_dbg(initial->on_freelist, "vm_page_unfree");

	dom->free_n[order]--;
	TAILQ_REMOVE(&dom->free_q[order], page, qlink);
	initial->on_freelist = false;
}

void
vmp_range_unfree(paddr_t base, paddr_t limit)
{
	base = rounddown2(base, PGSIZE);
	limit = roundup2(limit, PGSIZE);

	for (size_t aff_i = 0; aff_i < naffinities; aff_i++) {
		struct vm_affinity *aff = &affinities[aff_i];
		paddr_t region_base, region_limit;

		if (limit <= aff->base || base >= aff->limit)
			continue;

		region_base = MAX2(base, aff->base);
		region_limit = MIN2(limit, aff->limit);

		for (paddr_t i = region_base; i < region_limit;) {
			vm_page_t *page = VM_PAGE_FOR_PADDR(i);
			size_t order = MIN2(FREELIST_ORDERS - 1,
			    __builtin_ctz(i / PGSIZE));

			while ((i + (1 << order) * PGSIZE) > region_limit)
				order--;

			vmp_page_unfree(page, order);
			i += (1 << order) * PGSIZE;
		}
	}
}
