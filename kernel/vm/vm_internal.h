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
 * Cleanliness of a vmp_objpage. Pages go onto the dirty queue as soon as they
 * are mapped writeable, and this queue is periodically processed to move pages
 * onto the clean queue by testing whether they are really dirty. The pages have
 * all their write mappings made away with and if they are dirty, they must be
 * written to disk.
 *
 * Pages thus migrate slowly to the clean queue.
 */
enum vmp_objpage_status {
	kVMPObjPageCreated,
	kVMPObjPageDirty,
	kVMPObjPageClean,
	kVMPObjPageCleaning,
};

/*!
 * vmp_objpage cleanliness queue type
 */
TAILQ_HEAD(vmp_objpage_dirty_queue, vmp_objpage);

/*!
 * Page within a VM object (i.e. a vnode). Locked by VM object's lock.
 */
struct vmp_objpage {
	/*! Linkage in vm_object::page_ref_rbtree */
	RB_ENTRY(vmp_objpage) rbtree_entry;
	/*! Linkage in maybe-dirty, dirty, or clean queue. */
	TAILQ_ENTRY(vmp_objpage) dirtyqueue_entry;
	/*! Where in the object does it belong to? (Page number!) */
	uint64_t page_index : 48;
	enum vmp_objpage_status stat : 4;
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

/*! @brief Free a pmap (the map should already be empty.) */
void pmap_free(vm_map_t *map);

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
 * @brief Remove writeable mappings of a page and return its dirtiness.
 * \pre PQ lock held.
 */
bool pmap_pageable_undirty(vm_page_t *page);

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

/*! @brief Invalidate a virtual address everywhere. */
void pmap_global_invlpg(vaddr_t vaddr);

/*!
 * @brief Find the map entry in a process to which a virtual address belongs.
 *
 * @pre Map mutex locked.
 */
vm_map_entry_t *vmp_map_find(vm_map_t *ps, vaddr_t vaddr);

/*! @brief Inform the cleaner of a new (clean) object page. */
void vmp_objpage_created(struct vmp_objpage *opage);

/*! @brief Inform the cleaner an object page is potentially dirty. */
void vmp_objpage_dirty(vm_object_t *obj, struct vmp_objpage *opage);

/*! @brief Free an object page. Must be clean & object must be LOCKED. */
void vmp_objpage_free(vm_map_t *map, struct vmp_objpage *opage);

/*! @brief Initialise an amap. */
int vmp_amap_init(vm_map_t *map, struct vm_amap *amap);

/*! @brief Free an amap. */
int vmp_amap_free(vm_map_t *map, struct vm_amap *amap);

/*! @brief Release a reference to an anon, optionally releasing held mutex. */
int vmp_anon_release(vm_map_t *map, struct vmp_anon *anon, bool mutex_held);

/*! @brief Dump all entries in a map. */
void vmp_map_dump(vm_map_t *map);

/*! @brief Allocate a page when PFN DB lock is held. */
int vmp_page_alloc_locked(vm_map_t *ps, bool must, enum vm_page_use use,
    vm_page_t **out);

/*! @brief Initialise pagedaemon. */
void vm_pdaemon_init(void);

/*! @brief Pagedaemon thread loop. */
void vm_pagedaemon(void);

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
