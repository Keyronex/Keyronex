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

#include <sys/krx_file.h>
#include <sys/krx_vfs.h>
#include <sys/kmem.h>
#include <sys/vnode.h>

#include <stdbool.h>

file_t *
file_new(namecache_handle_t nch, vnode_t *vn, int flags)
{
	file_t *file;

	file = kmem_alloc(sizeof(file_t));
	if (file == NULL) {
		return NULL;
	}

	__atomic_store_n(&file->refcnt, 1, __ATOMIC_RELAXED);
	file->nch = nch;
	file->vnode = vn;
	ke_mutex_init(&file->offset_mutex);
	file->offset = 0;
	file->flags = flags;

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
			return f;
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

void
file_release(file_t *file)
{
	uint32_t old = __atomic_fetch_sub(&file->refcnt, 1, __ATOMIC_ACQ_REL);

	if (old == 1) {
		if (file->nch.nc != NULL) {
			if (file->nch.nc->vp->ops->close != NULL)
				VOP_CLOSE(file->nch.nc->vp, file->flags);
			nchandle_release(file->nch);
		} else if (file->vnode != NULL) {
			if (file->vnode->ops->close != NULL)
				VOP_CLOSE(file->vnode, file->flags);
			vn_release(file->vnode);
		}
	}
}
