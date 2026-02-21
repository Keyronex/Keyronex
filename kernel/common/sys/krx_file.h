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
	kmutex_t offset_mutex;
	off_t offset;
	int flags;

	kspinlock_t epoll_lock;
	LIST_HEAD(, poll_entry) epoll_watches;
} file_t;

file_t *file_new(namecache_handle_t, struct vnode *);

file_t *file_tryretain_rcu(file_t *file);
void file_release(file_t *file);

int sys_close(int fd);
ssize_t sys_read(int fd, void *ubuf, size_t nbyte);
ssize_t sys_write(int fd, const void *ubuf, size_t nbyte);
int sys_lseek(int fd, off_t offset, int whence, off_t *out);
int sys_ioctl(int fd, int cmd, intptr_t arg);

#endif /* ECX_SYS_KRX_FILE_H */
