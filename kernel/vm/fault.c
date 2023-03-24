/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 17 2023.
 */
/*!
 * @file vm/fault.c
 * @brief Page fault handling.
 *
 * General note: If `out` is specified in any of these functions, the page will
 * be written to there with its reference count incremented.
 *
 * note: we may want a "wsl_update_protection"? instead of wsl_remove and
 * wsl_insert always. This will let us deal with the case of wired pages, which
 * can't be removed.
 */

#include "abi-bits/errno.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "vm/vm_internal.h"

static vm_fault_return_t fault_vnode(vm_map_t *map, vaddr_t vaddr,
    vm_map_entry_t *entry, vm_object_t *obj, voff_t offset,
    vm_fault_flags_t flags, vm_page_t **out, bool map_readonly);

RB_GENERATE(vmp_objpage_rbtree, vmp_objpage, rbtree_entry, vmp_objpage_cmp);

int
vmp_objpage_cmp(struct vmp_objpage *x, struct vmp_objpage *y)
{
	return x->page_index - y->page_index;
}

int
vmp_amap_init(vm_map_t *map, struct vm_amap *amap)
{
	vm_page_t *page;
	vmp_page_alloc(map, true, kPageUseVMM, &page);
	amap->l3 = VM_PAGE_DIRECT_MAP_ADDR(page);
	ke_mutex_init(&amap->mutex);
	return 0;
}

int
vmp_amap_descend(vm_map_t *map, struct vm_amap *amap, voff_t offset,
    krx_out struct vmp_anon ***panon, bool create)
{
	uint64_t pageoff = offset / 4096;
	uint16_t l3i = (pageoff >> 18) & 511;
	uint16_t l2i = (pageoff >> 9) & 511;
	uint16_t l1i = pageoff & 511;
	struct vmp_amap_l2 *l2;
	struct vmp_amap_l1 *l1;

	l2 = amap->l3->entries[l3i];
	if (!l2) {
		if (create) {
			vm_page_t *page;
			vmp_page_alloc(NULL, true, kPageUseVMM, &page);
			l2 = VM_PAGE_DIRECT_MAP_ADDR(page);
			amap->l3->entries[l3i] = l2;
		} else {
			return -ENOENT;
		}
	}

	l1 = l2->entries[l2i];
	if (!l1) {
		if (create) {
			vm_page_t *page;
			vmp_page_alloc(NULL, true, kPageUseVMM, &page);
			l1 = VM_PAGE_DIRECT_MAP_ADDR(page);
			l2->entries[l2i] = l1;
		} else {
			return -ENOENT;
		}
	}

	*panon = &l1->entries[l1i];

	return 0;
}

/*! @brief Find anon in an amap by its byte offset. */
int
vmp_amap_find_anon(vm_map_t *map, struct vm_amap *amap, voff_t offset,
    krx_out struct vmp_anon ***panon)
{
	return vmp_amap_descend(map, amap, offset, panon, false);
}

/*! @brief Insert an anon into an amap. */
void
vmp_amap_insert_anon(vm_map_t *map, struct vm_amap *amap, struct vmp_anon *anon)
{
	struct vmp_anon **panon;
	int r;

	r = vmp_amap_descend(map, amap, anon->offset, &panon, true);
	kassert(r == 0);
	kassert(*panon == NULL);

	*panon = anon;
}

/*! @brief Create a new anon, its page is wired and refcnt is 1. */
int
vmp_anon_new(vm_map_t *map, krx_out struct vmp_anon **out, voff_t offset)
{
	vm_page_t *page;
	struct vmp_anon *anon;
	int r;

	r = vmp_page_alloc(map, false, kPageUseAnonymous, &page);
	if (r != 0)
		return r;

	anon = kmem_alloc(sizeof(*anon));
	ke_mutex_init(&anon->mutex);
	anon->offset = offset;
	anon->page = page;
	anon->resident = true;
	anon->refcnt = 1;

	*out = anon;
	return 0;
}

/*!
 * @pre \p aobj lock held if aobj not NULL
 * @pre \p parent lock not taken
 */
static vm_fault_return_t
fault_anonymous(vm_map_t *map, vaddr_t vaddr, vm_map_entry_t *entry,
    struct vm_amap *amap, vm_object_t *aobj, vm_object_t *parent, voff_t offset,
    vm_fault_flags_t flags, vm_page_t **out)
{
	struct vmp_anon *anon = NULL, **panon = NULL;
	kwaitstatus_t w;

	w = ke_wait(&amap->mutex, "fault_anonymous:amap->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	(void)vmp_amap_find_anon(map, amap, offset, &panon);
	if (panon)
		anon = *panon;

	if (anon != NULL) {
		w = ke_wait(&anon->mutex, "fault_anonymous:anon->mutex", false,
		    false, -1);
		kassert(w == kKernWaitStatusOK);

		if (!anon->resident) {
			kfatal("implement anon pagein\n");
		}

		if (anon->refcnt > 1) {
			if (flags & kVMFaultWrite) {
				/*
				 * a write to an anon with refcnt >1 must always
				 * duplicate that anon.
				 */

				struct vmp_anon *new_anon;
				int ret;

				ret = vmp_anon_new(map, &new_anon, offset);
				kassert(ret == 0);

				memcpy(VM_PAGE_DIRECT_MAP_ADDR(new_anon->page),
				    anon->page, PGSIZE);

				/* unmap any old entry */
				pmap_unenter_pageable(map, NULL, vaddr);
				anon->refcnt--;

				ke_mutex_release(&anon->mutex);

				/* replace the amap entry with the new anon */
				anon = new_anon;
				*panon = new_anon;

				/* and map the newly-copied anon */
				pmap_enter_pageable(map, new_anon->page, vaddr,
				    entry->protection);

				if (out) {
					/* still wired from vmp_anon_new */
					*out = new_anon->page;
				} else {
					vm_page_unwire(new_anon->page);
				}

				/* note: new anon wasn't locked */
			} else {
				/*
				 * a read of an anon with a refcnt >1 can be
				 * satisfied without duplicating the anon
				 */

				/* first remove any existing entry */
				pmap_unenter_pageable(map, NULL, vaddr);

				/* then map the anon readonly */
				pmap_enter_pageable(map, anon->page, vaddr,
				    kVMRead | kVMExecute);

				if (out) {
					vm_page_wire(anon->page);
					*out = anon->page;
				} else {
					vm_page_activate(anon->page);
				}

				ke_mutex_release(&anon->mutex);
			}
		} else {
			/*
			 * with a refcnt of 1, the anon can be straightforwardly
			 * mapped read-write. This case happens either:
			 * 1. during MDL population
			 * 2. when the page has been put onto a nonactive LRU
			 * queue.
			 * 3. when there was at least one CoW clone made and the
			 * other clones have been write-faulted already.
			 */

			/* first remove any existing entry */
			pmap_unenter_pageable(map, NULL, vaddr);

			/* then map the anon */
			pmap_enter_pageable(map, anon->page, vaddr,
			    entry->protection);

			if (out) {
				vm_page_wire(anon->page);
				*out = anon->page;
			} else {
				vm_page_activate(anon->page);
			}

			ke_mutex_release(&anon->mutex);
		}

		ke_mutex_release(&amap->mutex);

		return kVMFaultRetOK;
	} else if (parent != NULL) {
		vm_fault_return_t r;
		vm_page_t *page;
		vaddr_t parent_offset;

		/* can't fault from an anonymous parent */
		kassert(!parent->is_anonymous &&
		    parent->objhdr.type == kObjTypeVNode);
		/* anonymous objects don't have parents */
		kassert(!aobj);

		parent_offset = offset + entry->offset;

		w = ke_wait(&parent->mutex, "fault_anonymous:parent->mutex",
		    false, false, -1);
		kassert(w == kKernWaitStatusOK);

#if 0
		kdprintf(
		    " !! Anonymous fault from parent: Base %lx, Addr %lx, offset %lx\n",
		    entry->start, vaddr, offset);
#endif

		r = fault_vnode(map, vaddr, entry, parent, parent_offset, flags,
		    &page, true);
		if (r != kVMFaultRetOK) {
			if (aobj)
				ke_mutex_release(&aobj->mutex);
			ke_mutex_release(&amap->mutex);
			/* it already released map mutex and its object mutex
			 * for us */
			return r;
		}

		if (!(flags & kVMFaultWrite)) {
			if (out) {
				*out = page;
			} else {
				vm_page_unwire(page);
			}

			ke_mutex_release(&parent->mutex);
			ke_mutex_release(&amap->mutex);

			return kVMFaultRetOK;
		}

		/* it's a write mapping - need to duplicate */

		struct vmp_anon *anon;
		int ret;

		ret = vmp_anon_new(map, &anon, offset);
		if (ret != 0) {
			ke_mutex_release(&parent->mutex);
			ke_mutex_release(&amap->mutex);
			vm_page_unwire(page);
			kfatal("Unlock everything for a wait-for-free-pages\n");
		}

		vmp_page_copy(page, anon->page);
		vm_page_unwire(page);

		vmp_amap_insert_anon(map, amap, anon);

		/* first remove the existing entry */
		pmap_unenter_pageable(map, NULL, vaddr);

		ke_mutex_release(&parent->mutex);

		/* then map the anon  */
		pmap_enter_pageable(map, anon->page, vaddr, entry->protection);
		pmap_invlpg(vaddr);

		if (out) {
			/* still wired from vmp_anon_new */
			*out = anon->page;
		} else {
			vm_page_unwire(anon->page);
		}

		/* note: new anon wasn't locked */

		ke_mutex_release(&amap->mutex);

		return kVMFaultRetOK;
	} else {
		/*
		 * no anon and no parent - it's a simple demand-zero.
		 */

		struct vmp_anon *anon;
		int ret;

		ret = vmp_anon_new(map, &anon, offset);
		if (ret != 0) {
			kfatal("Unlock everything for a wait-for-free-pages\n");
		}

		vmp_amap_insert_anon(map, amap, anon);

		/* map the anon  */
		pmap_enter_pageable(map, anon->page, vaddr, entry->protection);

		if (out) {
			/* still wired from vmp_anon_new */
			*out = anon->page;
		} else {
			vm_page_unwire(anon->page);
		}

		ke_mutex_release(&amap->mutex);

		return kVMFaultRetOK;
	}
}

static vm_fault_return_t
fault_vnode(vm_map_t *map, vaddr_t vaddr, vm_map_entry_t *entry,
    vm_object_t *obj, voff_t offset, vm_fault_flags_t flags, vm_page_t **out,
    bool map_readonly)
{
	struct vmp_objpage *objpage, key;

	key.page_index = offset / PGSIZE;
	objpage = RB_FIND(vmp_objpage_rbtree, &obj->page_rbtree, &key);

	if (!objpage) {
		vm_page_t *page;
		int ret;

		ret = vmp_page_alloc(map, false, kPageUseObject, &page);
		kassert(ret == 0);

		page->status = kPageStatusBusy;
		page->obj = obj;

		objpage = kmem_alloc(sizeof(*objpage));
		objpage->page = page;
		objpage->page_index = key.page_index;
		RB_INSERT(vmp_objpage_rbtree, &obj->page_rbtree, objpage);

		/*
		 * now that the page is inserted into the object and busied, we
		 * can unlock everything and do the I/O.
		 */

		ke_mutex_release(&obj->mutex);
		ke_mutex_release(&map->mutex);

		vnode_t *vnode = (vnode_t *)obj;
		vm_mdl_t *mdl = vm_mdl_alloc(1);

#if 0
		/*! do we need a reader lock? */
		kwaitstatus_t w;
#endif

		mdl->pages[0] = page;
		iop_t *iop = iop_new_read(vnode->vfsp->dev, mdl, PGSIZE,
		    offset);
		iop->stack[0].vnode = vnode;
		iop_return_t res = iop_send_sync(iop);
		kassert(res == kIOPRetCompleted);

		/*
		 * the I/O having completed, we can change the page status from
		 * busy to wired, then do unwire, which will place it on the
		 * active queue.
		 */

		page->status = kPageStatusWired;
		vm_page_unwire(page);

		/*
		 * finally we refault, as we have no guarantees of consistency
		 * after having unlocked for I/O.
		 */

		return kVMFaultRetRetry;
	} else if (objpage->page->status == kPageStatusBusy) {
		kfatal("Unlock everything & wait for busy page!\n");
	} else {
		vm_page_t *page = objpage->page;

		kassert(page->status != kPageStatusBusy);

		if (out) {
			vm_page_wire(page);
			*out = page;
		} else {
			vm_page_activate(page);
		}

		/* remove any existing entry */
		pmap_unenter_pageable(map, NULL, vaddr);

		pmap_enter_pageable(map, page, vaddr,
		    map_readonly ? (kVMRead | kVMExecute) : kVMAll);
		return kVMFaultRetOK;
	}
}

static vm_fault_return_t
fault_object(vm_map_t *map, vaddr_t vaddr, vm_map_entry_t *entry, voff_t offset,
    vm_fault_flags_t flags, vm_page_t **out)
{
	vm_object_t *obj = entry->object;
	kwaitstatus_t w;
	vm_fault_return_t r;

	w = ke_wait(&obj->mutex, "fault_obj:obj->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	if (obj->is_anonymous) {
		r = fault_anonymous(map, vaddr, entry, &obj->amap, obj, NULL,
		    offset, flags, out);
	} else {
		kassert(obj->objhdr.type == kObjTypeVNode);
		r = fault_vnode(map, vaddr, entry, obj, offset, flags, out,
		    false);
	}

	if (r != kVMFaultRetRetry)
		ke_mutex_release(&obj->mutex);

	return r;
}

vm_fault_return_t
vm_fault(vm_map_t *map, vaddr_t vaddr, vm_fault_flags_t flags, vm_page_t **out)
{
	vm_fault_return_t r;
	kwaitstatus_t w;
	vm_map_entry_t *entry;
	voff_t offset;

	if (vaddr >= HHDM_BASE) {
		if (flags & kVMFaultUser) {
			kfatal("User fault in kernel space.\n");
		}
		map = kernel_process.map;
	}

	w = ke_wait(&map->mutex, "vm_fault:map->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	entry = vmp_map_find(map, vaddr);
	if (entry == NULL) {
		kdprintf("vm_fault: map %p no entry at address 0x%lx\n", map,
		    vaddr);
		vmp_map_dump(map);
		return kVMFaultRetFailure;
	}

	/* verify protection permits */
	if (flags & kVMFaultWrite && !(entry->protection & kVMWrite)) {
		kfatal(
		    "vm_fault: write fault at 0x%lx in non-writeable entry\n",
		    vaddr);
	}

	offset = vaddr - entry->start;

	if (entry->has_anonymous) {
		r = fault_anonymous(map, vaddr, entry, &entry->amap, NULL,
		    entry->object, offset, flags, out);
	} else {
		r = fault_object(map, vaddr, entry, offset + entry->offset,
		    flags, out);
	}

	if (r != kVMFaultRetOK) {
		/* locks have already been dropped */
		return r;
	}

	ke_mutex_release(&map->mutex);

	return r;
}