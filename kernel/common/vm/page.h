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

#include <sys/k_intr.h>
#include <sys/vm_types.h>
#include <sys/vm.h>

#include <libkern/queue.h>

#define FREELIST_ORDERS 16
#define PAGEABLE_ORDERS 4

typedef TAILQ_HEAD(vm_page_queue, vm_page) vm_page_queue_t;
typedef struct vm_domain vm_domain_t;

/*
 * The page struct.
 * Locking:
 * - D: Domain's queues lock.
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

	/* Some use specific state (o) */
	union {
		struct proctable {
			/*
			 * These elements are all guarded by the owner's
			 * stealing_lock.
			 *
			 * noswap_ptes: number of PTEs in table that forbid the
			 * table from being swapped out, i.e. valid+trans. Swap
			 * and fork PTEs don't count towards this.
			 *
			 * nonzero_ptes: number of PTEs of any nonzero kind.
			 *
			 * valid_pageable_leaf_ptes: number of valid, pageable,
			 * leaf PTEs. This is only kept for tables containing
			 * pageable leaf PTEs.
			 *
			 * level: page table level (0 = furthest from root)
			 */
			int16_t noswap_ptes; /* aka share_count */
			int16_t nonzero_ptes;
			int16_t valid_pageable_leaf_ptes;
			int16_t spare_1;
			uintptr_t
				base: PFN_BITS,
				level: 3,
				is_root: 1;
		} proctable;

		struct objtable {
			int16_t noswap_ptes; /* aka share_count */
			int16_t nonzero_ptes;
			uintptr_t
				level: 3,
				is_root: 1;
		} objtable;

		/* anon shared, file shared, forked anon */
		struct shared {
			uint32_t share_count;
			uint32_t
				dirty: 1,
				spare_1: 31;
			uint64_t
				spare_2: 12,
				offset: 52; /* offset in *pages* */
		} shared;

		struct anon_priv {
		} anon_priv;
	};

	union {
		/* Paging queue membership (D) or valid leaf tables link (o) */
		TAILQ_ENTRY(vm_page) qlink;

		/* Pagein wait head (o) */
		struct pagein_wait *pagein_wait;
	};

	/* PTE that maps this page (o)*/
	union pte *pte;

	/* Owner (D) */
	union {
		struct vm_rs *owner_rs;
		struct vm_object *owner_obj;
		struct vm_anon *owner_anon;
	};

	union {
		/* Swap location (D?) */
		uintptr_t swap_address;
	};

#if PFN_BITS == 20
	uint32_t padding;
#endif
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

void vmp_page_dom_lock_enter(vm_page_t *);
void vmp_page_dom_lock_exit(vm_page_t *);

void vm_page_retain(vm_page_t *);
void vm_page_release(vm_page_t *);
void vm_page_release_and_dirty(vm_page_t *page, bool dirty);
void vm_page_dirty(vm_page_t *);

extern vm_domain_t vm_domains[1];
extern vm_page_t *vm_pages;

#endif /* ECX_VM_PAGE_H */
