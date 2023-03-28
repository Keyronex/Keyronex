/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */

#ifndef KRX_VM_VM_INTERNAL_H
#define KRX_VM_VM_INTERNAL_H

#include "kdk/kernel.h"
#include "kdk/vm.h"

/*! How many pages are in an amap chunk? */
#define kAMapChunkNPages 32

/*!
 * Page within a VM object (i.e. a vnode). Locked by VM object's lock.
 */
struct vmp_objpage {
	/*! Linkage in vm_object::page_ref_rbtree */
	RB_ENTRY(vmp_objpage) rbtree_entry;
	/*! Where in the object does it belong to? (Page number!) */
	size_t page_index;
	/*! Underlying page.*/
	vm_page_t *page;
};

/*! An anonymous page. */
struct vmp_anon {
	kmutex_t mutex;
	union {
		vm_page_t *page;
		uintptr_t drumslot;
	};
	voff_t offset;
	uint32_t refcnt;
	bool resident : 1;
};

/*! vm_amap level 3 table. */
struct vmp_amap_l3 {
	struct vmp_amap_l2 *entries[512];
};

/*! vm_amap level 2 table. */
struct vmp_amap_l2 {
	struct vmp_amap_l1 *entries[512];
};

/*! vm_amap level 1 table. */
struct vmp_amap_l1 {
	struct vmp_anon *entries[512];
};

/*! @brief Initialise the pmap of a newly created map. */
void pmap_new(struct vm_map *map);

/*! @brief Enter a page mapping. */
void pmap_enter(vm_map_t *map, paddr_t phys, vaddr_t virt,
    vm_protection_t prot);

/*! @brief Enter a pageable page mapping. */
void pmap_enter_pageable(vm_map_t *map, vm_page_t *page, vaddr_t virt,
    vm_protection_t prot);

/*! @brief Remove a page mapping, returning the page previously mapped. */
vm_page_t *pmap_unenter(vm_map_t *map, vaddr_t vaddr);

/*! @brief Remove a pageable mapping, returning any page previously mapped. */
int pmap_unenter_pageable(vm_map_t *map, krx_out vm_page_t **page,
    vaddr_t virt);

/*! @brief Remove a range of pageable mappings. */
void pmap_unenter_pageable_range(vm_map_t *map, vaddr_t start, vaddr_t end);

/*! @brief Translate virtual address to physical. */
paddr_t pmap_trans(vm_map_t *map, vaddr_t virt);

/*! @brief Locally invalidate a mapping. */
void pmap_invlpg(vaddr_t vaddr);

/*!
 * @brief Reduce protections on all mappings within some range of memory.
 */
void pmap_protect_range(vm_map_t *map, vaddr_t base, vaddr_t end,
    vm_protection_t limit);

/*!
 * @brief Check if a page is present in a process.
 * @param paddr if this is non-NULL and the page is present, the page's physical
 * address will be written here.
 */
bool pmap_is_present(vm_map_t *map, vaddr_t vaddr, paddr_t *paddr);

/*!
 * @brief Check if a page is writeably mapped in a process.
 * @param paddr if this is non-NULL and the page is writeably mapped, the page's
 * physical address will be written here.
 */
bool pmap_is_writeable(vm_map_t *map, vaddr_t vaddr, paddr_t *paddr);

/*!
 * @brief Find the map entry in a process to which a virtual address belongs.
 *
 * @pre Map mutex locked.
 */
vm_map_entry_t *vmp_map_find(vm_map_t *ps, vaddr_t vaddr);

/*! @brief Initialise an amap. */
int vmp_amap_init(vm_map_t *map, struct vm_amap *amap);

/*! @brief Dump all entries in a map. */
void vmp_map_dump(vm_map_t *map);

/*! @brief Allocate a page when PFN DB lock is held. */
int vmp_page_alloc_locked(vm_map_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out);

/*! @brief Comparator function for map entry rb-tree. */
int vmp_map_entry_cmp(vm_map_entry_t *x, vm_map_entry_t *y);
/*! @brief Comparator function for page ref rb-tree. */
int vmp_objpage_cmp(struct vmp_objpage *x, struct vmp_objpage *y);
/*! @brief Comparator function for anon rb-tree. */
int vmp_anon_cmp(struct vmp_anon *x, struct vmp_anon *y);

RB_PROTOTYPE(vm_map_entry_rbtree, vm_map_entry, rbtree_entry,
    vmp_map_entry_cmp);
RB_PROTOTYPE(vmp_objpage_rbtree, vmp_objpage, rbtree_entry, vmp_objpage_cmp);

#endif /* KRX_VM_VM_INTERNAL_H */
