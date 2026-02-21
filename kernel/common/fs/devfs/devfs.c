/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file devfs.c
 * @brief Device filesystem
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/krx_vfs.h>
#include <sys/libkern.h>
#include <sys/vnode.h>

#include <fs/devfs/devfs.h>
#include "sys/strsubr.h"

#define VTODN(VN) ((dev_node_t *)(VN)->fsprivate_1)
static struct vnode_ops dev_spec_vnops, dev_vnops;
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
	node->vn = NULL;

	node->devprivate = private;
	node->stdata = NULL;

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

	return vn_alloc(NULL, VCHR, &dev_spec_vnops, (uintptr_t)node, 0);
}

static int
dev_lookup(vnode_t *dvn, const char *name, vnode_t **out)
{
	dev_node_t *dn = NULL;

	if (strcmp(name, ".") == 0) {
		*out = dvn;
		vn_retain(dvn);
		return 0;
	}
	if (strcmp(name, "..") == 0) {
		kfatal("unimplemented");
	}

	ke_rwlock_enter_read(&hash_lock, "devfs_dir_lookup");
	TAILQ_FOREACH(dn, &hash, hash_entry) {
		if (strcmp(dn->name, name) == 0)
			break;
	}
	ke_rwlock_exit_read(&hash_lock);

	if (dn == NULL)
		return -ENOENT;

	ke_rwlock_enter_write(&dn->open_lock, "dev_lookup node");
	if (dn->vn == NULL)
		dn->vn = vn_alloc(NULL, VCHR, &dev_spec_vnops,
		    (uintptr_t)dn, 0);
	ke_rwlock_exit_write(&dn->open_lock);

	*out = dn->vn;

	return 0;
}

static int
dev_spec_inactive(vnode_t *vn)
{
	kfatal("dev_inactive");
}

static int
dev_spec_open(vnode_t **vn, int)
{
	dev_node_t *dn = VTODN(*vn);

	ke_rwlock_enter_write(&dn->open_lock, "dev_spec_open");

	dn->open_count++;
	if (dn->open_count == 1) {
		ke_rwlock_exit_write(&dn->open_lock);
		return 0;
	}

	switch(dn->class->kind) {
		case DEV_KIND_STREAM:
			dn->stdata = stropen(dn->class->streamtab,
			    dn->devprivate);
			break;

		default:
			kfatal("unsupported dev kind %d in %s\n",
			    dn->class->kind, dn->name);
	}

	ke_rwlock_exit_write(&dn->open_lock);

	return 0;
}

int
dev_spec_read(vnode_t *vn, void *buf, size_t length, off_t offset, int flags)
{
	dev_node_t *dn = VTODN(vn);

	switch (dn->class->kind) {
	case DEV_KIND_STREAM:
		return strread(dn->stdata, buf, length, flags);

	default:
		kfatal("implement me");
	}
}

int
dev_spec_write(vnode_t *vn, const void *buf, size_t length, off_t offset,
    int flags)
{
	dev_node_t *dn = VTODN(vn);

	switch (dn->class->kind) {
	case DEV_KIND_STREAM:
		return strwrite(dn->stdata, buf, length, flags);

	default:
		kfatal("implement me");
	}
}

int
dev_spec_ioctl(vnode_t *vn, unsigned long cmd, void *arg)
{
	dev_node_t *dn = VTODN(vn);

	switch (dn->class->kind) {
	case DEV_KIND_STREAM:
		return strioctl(vn, dn->stdata, cmd, arg);

	default:
		kfatal("implement me");
	}
}

iop_return_t
dev_spec_iop_dispatch(vnode_t *vn, struct iop *iop)
{
	dev_node_t *dn = VTODN(vn);
	return dn->class->charops->iop_dispatch(dn->devprivate, iop);
}

iop_return_t
dev_spec_iop_complete(vnode_t *vn, struct iop *iop)
{
	dev_node_t *dn = VTODN(vn);
	return dn->class->charops->iop_complete(dn->devprivate, iop);
}

void
mount_devfs(void)
{
	int r;
	namecache_handle_t overnch;
	vnode_t *vn;
	vfs_t *vfs = kmem_alloc(sizeof(vfs_t));

	vfs_init(vfs);

	r = vfs_lookup_simple(root_nch, &overnch, "/dev", 0);
	if (r != 0)
		kfatal("devfs_mount: /dev not found\n");

	vn = vn_alloc(NULL, VDIR, &dev_vnops, 0, 0);

	void nc_domount(namecache_handle_t overnch, vfs_t * vfs,
	    vnode_t * rootvn);

	nc_domount(overnch, vfs, vn);
}

static struct vnode_ops dev_spec_vnops = {
	.inactive = dev_spec_inactive,
	.open = dev_spec_open,
	.read = dev_spec_read,
	.write = dev_spec_write,
	.ioctl = dev_spec_ioctl,
	.stack_depth = 2,
	.iop_dispatch = dev_spec_iop_dispatch,
	.iop_complete = dev_spec_iop_complete,
};

static struct vnode_ops dev_vnops = {
	.lookup = dev_lookup,
};
