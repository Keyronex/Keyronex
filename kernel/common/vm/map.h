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

#include <sys/k_thread.h>
#include <sys/tree.h>
#include <sys/vmem_impl.h>
#include <sys/pmap.h>
#include <sys/vm.h>

#include <vm/page.h>

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


typedef struct vm_object {
	kspinlock_t creation_lock;
	kspinlock_t stealing_lock;
	enum {
		VM_OBJ_VNODE,
		VM_OBJ_ANON,
	} kind;
	union {
		struct vnode *vnode;
	};
	pte_t direct[6];
	pte_t indirect[4]; /* [0] = indirect, [1] = doubly indirect, etc */
} vm_object_t;

struct vm_anon {
	pte_t pte;
	uintptr_t refcount;
};

struct obj_pte_wire_state {
	pte_t *pte;
	vm_page_t *pages[4];
	vaddr_t offset;
};

struct table_lock_state {
	kspinlock_t *lock;
	bool did_unlock;
};

struct vm_map_entry *vm_map_lookup(vm_map_t *map, vaddr_t addr);

int obj_wire_pte(vm_object_t *obj, struct obj_pte_wire_state *state,
    vaddr_t offset, bool create, struct table_lock_state *table_lock_state);
void obj_unwire_pte(vm_object_t *obj, struct obj_pte_wire_state *state);

pte_t *obj_fetch_pte(vm_object_t *obj, vaddr_t offset);

void obj_page_zeroed(vm_object_t *obj, vm_page_t *page);
void obj_page_swapped(vm_object_t *obj, vm_page_t *page);
void obj_table_pte_did_become_swap(vm_object_t *obj, vm_page_t *table_page);

void pmap_tlb_flush_vaddr_globally(vaddr_t vaddr);
void pmap_tlb_flush_all_globally(void);

void pmap_tlb_flush_all(void *unused);

void rs_evict_leaf_pte(struct vm_rs *rs, vaddr_t vaddr, vm_page_t *page,
    pte_t *pte);

extern kspinlock_t anon_creation_lock, anon_stealing_lock;
extern vm_map_t kernel_map;

#endif /* ECX_VM_MAP_H */
