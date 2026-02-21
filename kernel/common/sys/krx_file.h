/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file file.h
 * @brief File object
 */

#ifndef ECX_SYS_KRX_FILE_H
#define ECX_SYS_KRX_FILE_H

#include <sys/types.h>
#include <sys/k_thread.h>
#include <sys/krx_vfs.h>

#include <stdint.h>

struct vnode;

typedef struct file {
	uint32_t refcnt;
	namecache_handle_t nch;
	struct vnode *vnode;
	kspinlock_t offset_mutex;
	off_t offset;
	int flags;

	kspinlock_t epoll_lock;
	LIST_HEAD(, poll_entry) epoll_watches;
} file_t;

file_t *file_tryretain_rcu(file_t *file);
void file_release(file_t *file);

#endif /* ECX_SYS_KRX_FILE_H */
