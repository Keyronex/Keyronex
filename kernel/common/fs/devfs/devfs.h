/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file devfs.h
 * @brief Device filesystem - public header.
 */

#ifndef ECX_DEVFS_DEVFS_H
#define ECX_DEVFS_DEVFS_H

#include <sys/iop.h>
#include <sys/k_thread.h>

typedef struct dev_ops {
	int (*read)(void *dp, void *buf, size_t length, off_t,
	    int flags);
	int (*write)(void *dp, const void *buf, size_t length, off_t,
	    int flags);
	int (*ioctl)(void *dp, unsigned long cmd, void *data);
	int (*mmap)(void *addr, size_t len, int prot, int flags, void *dp,
	    off_t offset, vaddr_t *window);

	size_t stack_depth;
	iop_return_t (*iop_dispatch)(void *devprivate, struct iop *);
	iop_return_t (*iop_complete)(void *devprivate, struct iop *);
} dev_ops_t;

typedef struct dev_class {
	enum dev_kind {
		DEV_KIND_CHAR,
		DEV_KIND_CHAR_CLONE,
		DEV_KIND_STREAM,
		DEV_KIND_STREAM_CLONE,
	} kind;
	union {
		dev_ops_t *charops;
		struct streamtab *streamtab;
	};
} dev_class_t;

typedef struct dev_node {
	dev_class_t *class;
	TAILQ_ENTRY(dev_node) hash_entry;
	uint32_t open_count;
	krwlock_t open_lock; /* guarding open/close */
	void *devprivate;
	struct stdata *stdata;
	char name[16];
	struct vnode *vn;
} dev_node_t;

void devfs_create_node(dev_class_t *class, void *private, const char *fmt, ...);
struct vnode *devfs_lookup_early(const char *name);

#endif /* ECX_DEVFS_DEVFS_H */
