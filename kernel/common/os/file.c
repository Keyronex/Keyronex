/*
 * Copyright (c) 2024-2026 Cloudarox Solutions.
 * Created on Sat Jun 29 2024.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file file.c
 * @brief File object
 */

#include <sys/krx_epoll.h>
#include <sys/krx_file.h>
#include <sys/krx_vfs.h>
#include <sys/kmem.h>
#include <sys/vnode.h>

#include <stdbool.h>

atomic_uint files_allocated, files_freed;

file_t *
file_new(namecache_handle_t nch, vnode_t *vn, int flags)
{
	file_t *file;
	enum file_mode mode = 0;

	file = kmem_alloc(sizeof(file_t));
	if (file == NULL) {
		return NULL;
	}

	atomic_fetch_add_explicit(&files_allocated, 1, memory_order_relaxed);

	__atomic_store_n(&file->refcnt, 1, __ATOMIC_RELAXED);
	file->nch = nch;
	if (vn == NULL) {
		kassert(nch.nc != NULL);
		vn = vn_retain(nch.nc->vp);
	}
	file->vnode = vn;
	ke_mutex_init(&file->offset_mutex);
	file->offset = 0;

	atomic_store_explicit(&file->flags, flags, memory_order_release);

	switch (vn->type) {
	case VCHR:
	case VSOCK:
	case VFIFO:
		break;
	default:
		mode |= FILE_MODE_ATOMIC_OFFSET;
		break;
	}
	file->mode = mode;

	ke_spinlock_init(&file->epoll_lock);
	LIST_INIT(&file->epoll_watches);

	return file;
}

file_t *
file_tryretain_rcu(file_t *f)
{
	while (true) {
		uint32_t count = __atomic_load_n(&f->refcnt, __ATOMIC_RELAXED);
		if (count == 0)
			return NULL;
		if (__atomic_compare_exchange_n(&f->refcnt, &count, count + 1,
			false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			return f;
		}
	}
}

file_t *
file_retain(file_t *fp)
{
	fp = file_tryretain_rcu(fp);
	kassert(fp != NULL);
	return fp;
}

static void
file_free_rcu(void *arg)
{
	file_t *file = arg;

	kmem_free(file, sizeof(*file));
	atomic_fetch_add_explicit(&files_freed, 1, memory_order_relaxed);
}

void
file_release(file_t *file)
{
	uint32_t old = __atomic_fetch_sub(&file->refcnt, 1, __ATOMIC_ACQ_REL);

	kassert(old != 0, "file_release: refcount of file already 0");

	if (old == 1) {
		poll_watched_file_did_close(file);

		if (file->vnode->ops->close != NULL)
			VOP_CLOSE(file->vnode,
			    atomic_load_explicit(&file->flags,
				memory_order_relaxed));
		vn_release(file->vnode);
		if (file->nch.nc != NULL)
			nchandle_release(file->nch);

		ke_rcu_call(&file->rcu, file_free_rcu, file);
	}
}
