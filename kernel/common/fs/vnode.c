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
#include <sys/proc.h>
#include <sys/vnode.h>

#include <stdatomic.h>

atomic_uint vnodes_allocated, vnodes_freed;

static kspinlock_t vn_release_lock = KSPINLOCK_INITIALISER;
static TAILQ_HEAD(, vnode) vn_release_queue =
    TAILQ_HEAD_INITIALIZER(vn_release_queue);
static kevent_t vn_release_event;

vnode_t *
vn_alloc(vfs_t *vfs, vtype_t type, struct vnode_ops *ops, uintptr_t fsprivate_1,
    uintptr_t fsprivate_2)
{
	vnode_t *vn;

	atomic_fetch_add_explicit(&vnodes_allocated, 1, memory_order_relaxed);

	vn = kmem_alloc(sizeof(vnode_t));
	atomic_store_explicit(&vn->refcount, 1, memory_order_relaxed);
	vn->vfs = vfs;
	vn->type = type;
	vn->ops = ops;
	vn->fsprivate_1 = fsprivate_1;
	vn->fsprivate_2 = fsprivate_2;
	vn->published = false;
	vn->release_pending = 0;
	ke_spinlock_init(&vn->nc_lock);
	LIST_INIT(&vn->nc_refs);
	vn->nc_refs_n = 0;

	if (vn->type == VREG) {
		vn->file.vc_state = viewcache_alloc_vnode_state(vn);
		vn->file.vmobj = vm_obj_new_vnode(vn);
	}

	return vn;
}

void
vn_publish(vnode_t *vn)
{
	vfs_t *vfs = vn->vfs;
	ipl_t ipl;

	kassert(vfs != NULL, "vn_publish: vnode has no vfs");
	kassert(!vn->published, "vn_publish: vnode already published");

	ipl = ke_spinlock_enter(&vfs->vnode_list_lock);
	TAILQ_INSERT_TAIL(&vfs->vnode_list, vn, vfs_vnlistentry);
	vn->published = true;
	ke_spinlock_exit(&vfs->vnode_list_lock, ipl);
}

void
vn_free(vnode_t *vn)
{
	kassert(atomic_load_explicit(&vn->refcount, memory_order_acquire) == 0,
	    "vn_free: not frozen");
	kassert(vn->nc_refs_n == 0, "vn_free: still has namecache entries");
	kassert(vn->release_pending == 0, "vn_free: has pending releases");

	if (vn->type == VREG) {
		viewcache_free_vnode_state(vn);
		vm_obj_vnode_destroy(vn->file.vmobj);
	}

	if (vn->published) {
		vfs_t *vfs = vn->vfs;
		ipl_t ipl;

		ipl = ke_spinlock_enter(&vfs->vnode_list_lock);
		TAILQ_REMOVE(&vfs->vnode_list, vn, vfs_vnlistentry);
		ke_spinlock_exit(&vfs->vnode_list_lock, ipl);

		ke_waitq_wake_all(&vfs->vnode_waitq);
	}

	kmem_free(vn, sizeof(*vn));
	atomic_fetch_add_explicit(&vnodes_freed, 1, memory_order_relaxed);
}

vnode_t *
vn_retain(vnode_t *vn)
{
	uint32_t old = atomic_fetch_add_explicit(&vn->refcount, 1,
	    memory_order_relaxed);
	kassert(old != 0, "vn_retain of a frozen vnode");
	(void)old;
	return vn;
}

bool
vn_tryretain(vnode_t *vn)
{
	uint32_t old = atomic_load_explicit(&vn->refcount,
	    memory_order_relaxed);

	while (old != 0) {
		if (atomic_compare_exchange_weak_explicit(&vn->refcount, &old,
			old + 1, memory_order_acquire, memory_order_relaxed))
			return true;
	}

	return false;
}

bool
vn_tryfreeze(vnode_t *vn)
{
	uint32_t expected = 1;

	return atomic_compare_exchange_strong_explicit(&vn->refcount,
	    &expected, 0, memory_order_acq_rel, memory_order_relaxed);
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

			kassert(old == 1, "vn_release of a frozen vnode");
			kassert(vn->nc_refs_n == 0);

			kassert(vn->ops->inactive != NULL);
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
vn_release_async(vnode_t *vn)
{
	ipl_t ipl;

	ipl = ke_spinlock_enter(&vn_release_lock);
	if (vn->release_pending++ == 0)
		TAILQ_INSERT_TAIL(&vn_release_queue, vn, release_link);
	ke_spinlock_exit(&vn_release_lock, ipl);

	ke_event_set_signalled(&vn_release_event, true);
}

static void
vn_release_thread(void *)
{
	for (;;) {
		ke_wait1(&vn_release_event, "vn_release_thread", false,
		    ABSTIME_FOREVER);
		ke_event_set_signalled(&vn_release_event, false);

		for (;;) {
			vnode_t *vn;
			uint32_t n;
			ipl_t ipl;

			ipl = ke_spinlock_enter(&vn_release_lock);
			vn = TAILQ_FIRST(&vn_release_queue);
			if (vn == NULL) {
				ke_spinlock_exit(&vn_release_lock, ipl);
				break;
			}

			TAILQ_REMOVE(&vn_release_queue, vn, release_link);
			n = vn->release_pending;
			vn->release_pending = 0;
			ke_spinlock_exit(&vn_release_lock, ipl);

			while (n-- > 0)
				vn_release(vn);
		}
	}
}

void
vnode_init(void)
{
	thread_t *thread;

	ke_event_init(&vn_release_event, false);
	thread = proc_new_system_thread(vn_release_thread, NULL);
	if (thread == NULL)
		kfatal("couldn't create vnode release thread");
	ke_thread_resume(&thread->kthread, false);
}

void
vfs_init(vfs_t *vfs)
{
	ke_spinlock_init(&vfs->vnode_list_lock);
	TAILQ_INIT(&vfs->vnode_list);
	ke_waitq_init(&vfs->vnode_waitq);
	atomic_store_explicit(&vfs->opencnt, 2,
	    memory_order_relaxed); /* initial refcount; mountpoint ref */
}
