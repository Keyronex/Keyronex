/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Feb 13 2023.
 */
/*!
 * @file object/object.c
 * @brief Implements the object manager.
 *
 * The general principles:
 * - Objects can be referred to indirectly by handles, which are integers.
 * - Executive processes store a table of handle entries, pointing to objects.
 * - Objects always store an object header as their first element.
 */

#include "kdk/kernel.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/vfs.h"

#define DEBUG_ADDRESSES 1
#define DEBUG_VNODE 0

struct file;

void file_free(struct file *file);

void
obj_initialise_header(object_header_t *hdr, object_type_t type)
{
	hdr->type = type;
	hdr->reference_count = 1;
	hdr->name = NULL;
}

#if DEBUG_VNODE
static void
print_vnode_op(const char *op, vnode_t *vn)
{
	kdprintf(" ** %s vnode  %p (type %d mount %s path %s)\n", op, vn,
	    vn->type,
	    vn->vfsp ? vn->vfsp->mountpoint ? vn->vfsp->mountpoint : "?" : "?",
	    vn->path ? vn->path : "?");
}
#endif

void *
obj_retain(object_header_t *hdr)
{
#if DEBUG_ADDRESSES
	kassert((uintptr_t)hdr >= HHDM_BASE &&
	    (uintptr_t)hdr <= KHEAP_BASE + KHEAP_SIZE);
#endif

#if DEBUG_VNODE > 1
	if (hdr->type == kObjTypeVNode) {
		vnode_t *vn = (vnode_t *)hdr;
		print_vnode_op("reTAINing", vn);
	}
#endif

	__atomic_fetch_add(&hdr->reference_count, 1, __ATOMIC_SEQ_CST);
	return (void *)hdr;
}

void *
obj_direct_retain(void *obj)
{
	return obj_retain(obj);
}

void
obj_release(object_header_t *hdr)
{
	uint32_t old_cnt;

#if DEBUG_ADDRESSES
	kassert((uintptr_t)hdr >= HHDM_BASE &&
	    (uintptr_t)hdr <= KHEAP_BASE + KHEAP_SIZE);
#endif

#if DEBUG_VNODE > 1
	if (hdr->type == kObjTypeVNode)
		print_vnode_op("reLEASing", (vnode_t *)hdr);
#endif

	if (hdr->type == kObjTypeVNode) {
		/* will we actually need a  */
		if (__atomic_load_n(&hdr->reference_count, __ATOMIC_SEQ_CST) ==
		    1) {
			vnode_t *vn = (vnode_t *)hdr;

#if DEBUG_VNODE > 0
			print_vnode_op("INactivating", vn);
#endif

			if (vn == root_vnode || vn == dev_vnode)
				kfatal("Attempted to release %s vnode\n",
				    vn == root_vnode ? "root" : "/dev");
			if (vn->ops->inactive != NULL)
				vn->ops->inactive(vn);

			return;
		} else {
			old_cnt = __atomic_fetch_sub(&hdr->reference_count, 1,
			    __ATOMIC_SEQ_CST);
			kassert(old_cnt > 1 && old_cnt < 500);
			return;
		}
	}

	old_cnt = __atomic_fetch_sub(&hdr->reference_count, 1,
	    __ATOMIC_SEQ_CST);
	if (old_cnt == 1) {
		switch (hdr->type) {
		case kObjTypeFile:
			file_free((struct file *)hdr);
			break;

		default:
			break;
		}
#ifdef DEBUG_OBJ
		kdprintf("objmgr: <%p> (type %d) is to be freed\n", hdr,
		    hdr->type);
#endif
	}
}

void *
obj_direct_release(void *obj)
{
	obj_release(obj);
	return NULL;
}
