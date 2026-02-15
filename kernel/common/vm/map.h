/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-2026 Cloudarox Solutions.
 */
/*
 * @file vm/map.h
 * @brief VM map management private interfaces.
 */

#ifndef ECX_VM_MAP_H
#define ECX_VM_MAP_H

#include <keyronex/ktask.h>
#include <keyronex/tree.h>
#include <keyronex/vmem_impl.h>
#include <keyronex/vm.h>

#include "vm/page.h"

struct vm_map_entry {
	RB_ENTRY(vm_map_entry) rb_link;
	vaddr_t start, end;
	vm_prot_t prot, max_prot;
	bool inherit_shared, cow;
	size_t offset;
	bool is_phys;
	vm_cache_mode_t cache;
	paddr_t phys_base;
	struct vm_object *object;
};

RB_HEAD(vm_map_tree, vm_map_entry);
RB_PROTOTYPE(vm_map_tree, vm_map_entry, rb_link, map_entry_cmp);

/* Resident Set */
typedef struct vm_rs {
	struct vm_map *map;

	/* below are all guarded by map->stealing_lock */

	size_t private_pages_n;
	size_t valid_n;

	/* leaf page tables containing at least 1 valid, pageable PTE */
	TAILQ_HEAD(, vm_page) active_leaf_tables;
} vm_rs_t;

struct vm_map {
	atomic_uint refcnt;
	vmem_t vmem;
	krwlock_t map_lock;
	kspinlock_t creation_lock;
	kspinlock_t stealing_lock;
	struct vm_map_tree entries;

	paddr_t pgtable;
	struct vm_rs rs;
};

extern vm_map_t kernel_map;

#endif /* ECX_VM_MAP_H */
