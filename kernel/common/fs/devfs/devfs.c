/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file devfs.c
 * @brief Device filesystem
 */

#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/vnode.h>

#include <fs/devfs/devfs.h>
#include <libkern/lib.h>
#include <libkern/queue.h>

#define VTODN(VN) ((dev_node_t *)vn->fsprivate_1)
static struct vnode_ops dev_vnops;
static TAILQ_HEAD(, dev_node) hash = TAILQ_HEAD_INITIALIZER(hash);
static krwlock_t hash_lock;

void
devfs_create_node(dev_class_t *class, void *private, const char *fmt, ...)
{
	va_list ap;
	dev_node_t *node;

	node = kmem_alloc(sizeof(*node));

	node->class = class;
	node->open_count = 0;
	ke_rwlock_init(&node->open_lock);

	node->devprivate = private;

	va_start(ap, fmt);
	kvsnprintf(node->name, sizeof(node->name) - 1, fmt, ap);
	va_end(ap);

	ke_rwlock_enter_write(&hash_lock, "devfs_create_node");
	TAILQ_INSERT_TAIL(&hash, node, hash_entry);
	ke_rwlock_exit_write(&hash_lock);

	kdprintf("devfs_create_node: created %s\n", node->name);
}

vnode_t *
devfs_lookup_early(const char *name)
{
	dev_node_t *node;

	ke_rwlock_enter_read(&hash_lock, "devfs_lookup_early");
	TAILQ_FOREACH(node, &hash, hash_entry)
		if (strcmp(node->name, name) == 0)
			break;
	ke_rwlock_exit_read(&hash_lock);

	if (node == NULL)
		return NULL;

	return vn_alloc(NULL, VCHR, &dev_vnops, (uintptr_t)node, 0);
}

iop_return_t
dev_iop_dispatch(vnode_t *vn, struct iop *iop)
{
	dev_node_t *dn = VTODN(vn);
	return dn->class->charops->iop_dispatch(dn->devprivate, iop);
}

iop_return_t
dev_iop_complete(vnode_t *vn, struct iop *iop)
{
	dev_node_t *dn = VTODN(vn);
	return dn->class->charops->iop_complete(dn->devprivate, iop);
}

static struct vnode_ops dev_vnops = {
	.stack_depth = 2,
	.iop_dispatch = dev_iop_dispatch,
	.iop_complete = dev_iop_complete,
};
