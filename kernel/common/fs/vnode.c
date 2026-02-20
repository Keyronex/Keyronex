/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vnode.c
 * @brief Virtual node
 */

#include <sys/errno.h>
#include <sys/k_intr.h>
#include <sys/kmem.h>
#include <sys/krx_vfs.h>
#include <sys/vnode.h>

#include <stdatomic.h>

vnode_t *
vn_alloc(vfs_t *vfs, vtype_t type, struct vnode_ops *ops, uintptr_t fsprivate_1,
    uintptr_t fsprivate_2)
{
	vnode_t *vn;

	vn = kmem_alloc(sizeof(vnode_t));
	atomic_store_explicit(&vn->refcount, 1, memory_order_relaxed);
	vn->vfs = vfs;
	vn->type = type;
	vn->ops = ops;
	vn->fsprivate_1 = fsprivate_1;
	vn->fsprivate_2 = fsprivate_2;

	if (vn->type == VREG) {
		vn->file.vc_state = viewcache_alloc_vnode_state(vn);
		ke_spinlock_init(&vn->file.dpw_lock);
		vn->file.dpw_writers = 0;
		SLIST_INIT(&vn->file.dpw_waiters);
		vn->file.vmobj = vm_obj_new_vnode(vn);
	}

	if (vfs != NULL) {
		ipl_t ipl = ke_spinlock_enter(&vfs->vnode_list_lock);
		TAILQ_INSERT_TAIL(&vfs->vnode_list, vn, vfs_vnlistentry);
		ke_spinlock_exit(&vfs->vnode_list_lock, ipl);
	}

	return vn;
}

vnode_t *vn_retain(vnode_t *vn)
{
	atomic_fetch_add_explicit(&vn->refcount, 1, memory_order_relaxed);
	return vn;
}

void
vn_release(vnode_t *vn)
{
	uint32_t old = atomic_load_explicit(&vn->refcount,
	    memory_order_relaxed);

	while (true) {
		if (old > 1) {
			if (atomic_compare_exchange_weak_explicit(&vn->refcount,
				&old, old - 1, memory_order_release,
				memory_order_relaxed)) {
				return;
			} else {
				continue;
			}
		} else {
			int r;

			kassert(old == 1);

			r = vn->ops->inactive(vn);
			if (r == -EAGAIN) {
				/* retry release */
				old = atomic_load_explicit(&vn->refcount,
				    memory_order_relaxed);
				continue;
			} else {
				kassert(r == 0);
				return;
			}
		}
	}
}

void
vfs_init(vfs_t *vfs)
{
	ke_spinlock_init(&vfs->vnode_list_lock);
	TAILQ_INIT(&vfs->vnode_list);
	atomic_store_explicit(&vfs->opencnt, 2,
	    memory_order_relaxed); /* initial refcount; mountpoint ref */
}
