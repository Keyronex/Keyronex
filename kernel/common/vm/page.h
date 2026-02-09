/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024 Cloudarox Solutions.
 */
/*!
 * @file vm/page.h
 * @brief Resident page description.
 */

#ifndef ECX_VM_PAGE_H
#define ECX_VM_PAGE_H

#include <keyronex/intr.h>
#include <keyronex/vm_types.h>
#include <keyronex/vm.h>

#include <libkern/queue.h>

#define FREELIST_ORDERS 16
#define PAGEABLE_ORDERS 4

typedef TAILQ_HEAD(vm_page_queue, vm_page) vm_page_queue_t;
typedef struct vm_domain vm_domain_t;

/*
 * The page struct.
 * Locking:
 * - D: Domain queues lock.
 * - o: Owner lock (vm_rs or vm_object)
 */
struct vm_page {
	/* PMM state (D)*/
	uint32_t
	    max_order : 5,
	    order : 5,
	    on_freelist : 1,
	    use : 5,
	    dirty : 1,
	    domain : 4,
	    spare_1: 11;
	uint16_t ref_count;
	uint16_t spare_2;

	uintptr_t space[7];
};

struct vm_domain {
	kspinlock_t queues_lock;
	vm_page_queue_t free_q[FREELIST_ORDERS], stby_q, dirty_q;
	size_t free_n[FREELIST_ORDERS], stby_n, dirty_n, active_n;
	size_t use_n[VM_PAGE_USE_N];
};

/*! @brief get the pfn some vm_page describes */
#define VM_PAGE_PFN(page) (page - vm_pages)

/*! @brief get the paddr some vm_page describes */
#define VM_PAGE_PADDR(page) ((page - vm_pages) << PGSHIFT)

/*! @brief get the vm_page that describes some physical address. */
#define VM_PAGE_FOR_PADDR(addr) (&vm_pages[addr >> PGSHIFT])

/*! @brief get the vm_page that describes some HHDM address. */
#define VM_PAGE_FOR_HHDM_ADDR(addr) (&vm_pages[v2p(addr) >> PGSHIFT])

void vmp_page_dom_lock_enter(vm_page_t *page);
void vmp_page_dom_lock_exit(vm_page_t *page);

extern vm_domain_t vm_domain;
extern vm_page_t *vm_pages;

#endif /* ECX_VM_PAGE_H */
