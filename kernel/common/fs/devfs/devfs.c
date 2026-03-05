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
devfs_create_node(enum dev_kind kind, dev_ops_t *ops, void *private,
    const char *fmt, ...)
{
	va_list ap;
	dev_node_t *node;

	node = kmem_alloc(sizeof(*node));

	node->kind = kind;
	node->ops = ops;
	node->open_count = 0;
	node->is_anon_clone = false;
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

stdata_t *
devfs_spec_get_stream(vnode_t *vn)
{
	dev_node_t *dn;
	if (vn->ops != &dev_spec_vnops)
		return NULL;
	dn = VTODN(vn);
	if (dn->kind != DEV_KIND_STREAM)
		return NULL;
	return dn->stdata;
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

int
dev_getattr(vnode_t *vn, vattr_t *attr)
{
	dev_node_t *dn = VTODN(vn);
	memset(attr, 0x0, sizeof(*attr));
	attr->type = VDIR;
	attr->fileid = (uintptr_t)dn - HHDM_BASE;
	attr->fsid = (uintptr_t)vn->vfs;
	return 0;
}

/*
 * special ops
 */

static int
dev_spec_open(vnode_t **vn, int)
{
	dev_node_t *dn = VTODN(*vn);

	ke_rwlock_enter_write(&dn->open_lock, "dev_spec_open");

	switch (dn->kind) {
	case DEV_KIND_STREAM_CLONE: {
		dev_node_t *clone_dn = kmem_alloc(sizeof(*clone_dn));
		vnode_t *clone_vn;

		clone_dn->kind = DEV_KIND_STREAM;
		clone_dn->is_anon_clone = true;
		clone_dn->ops = dn->ops;
		clone_dn->open_count = 1;
		ke_rwlock_init(&clone_dn->open_lock);
		clone_dn->devprivate = dn->devprivate;

		ksnprintf(clone_dn->name, sizeof(clone_dn->name) - 1,
		    "clone:%s", dn->name);

		clone_dn->stdata = stropen(clone_dn->ops->streamtab,
		    clone_dn->devprivate, STR_HEAD_KIND_NONE);

		clone_vn = vn_alloc(NULL, VCHR, &dev_spec_vnops,
		    (uintptr_t)clone_dn, 0);
		clone_dn->vn = clone_vn;

		ke_rwlock_exit_write(&dn->open_lock);

		vn_release(*vn);
		*vn = clone_vn;
		return 0;
	}

	case DEV_KIND_CHAR_CLONE:
		ktodo();
		break;

	default:
		/* fall out */
		break;
	}

	/* non-clone open handling */

	dn->open_count++;
	if (dn->open_count > 1) {
		ke_rwlock_exit_write(&dn->open_lock);
		return 0;
	}

	switch(dn->kind) {
		case DEV_KIND_CHAR:
			kdprintf("todo: dev_spec_open for char\n");
			break;

		case DEV_KIND_STREAM:
			dn->stdata = stropen(dn->ops->streamtab,
			    dn->devprivate, STR_HEAD_KIND_TTY);

			if (dn->ops->autopush != NULL)
				strpush(dn->stdata, dn->ops->autopush);
			break;

		default:
			kfatal("unsupported dev kind %d in %s\n",
			    dn->kind, dn->name);
	}

	ke_rwlock_exit_write(&dn->open_lock);

	return 0;
}


static int
dev_spec_inactive(vnode_t *vn)
{
	dev_node_t *dn = VTODN(vn);

	ke_rwlock_enter_write(&dn->open_lock, "dev_lookup node");
	kassert(dn->vn != NULL);
	if (vn->refcount != 1) {
		ke_rwlock_exit_write(&dn->open_lock);
		return -EAGAIN;
	}

	dn->vn = NULL;
	vn->fsprivate_1 = 0;
	ke_rwlock_exit_write(&dn->open_lock);

	return 0;
}

static int
dev_spec_close(vnode_t *vn, int)
{
	dev_node_t *dn = VTODN(vn);

	ke_rwlock_enter_write(&dn->open_lock, "dev_spec_inactive");

	kassert(dn->open_count > 0);
	if (--dn->open_count == 0) {
		kfatal("dev_spec_close\n");

		switch (dn->kind) {
		case DEV_KIND_CHAR:
			kdprintf("todo: dev_spec_close for char\n");
			break;

		case DEV_KIND_STREAM:
			strclose(dn->stdata);
			dn->stdata = NULL;
			break;

		default:
			kfatal("dev_spec_close unexpected kind %d (named %s)\n",
			    dn->kind, dn->name);
		}
	}

	ke_rwlock_exit_write(&dn->open_lock);

	return 0;
}

int
dev_spec_getattr(vnode_t *vn, vattr_t *attr)
{
	dev_node_t *dn = VTODN(vn);
	memset(attr, 0x0, sizeof(*attr));
	attr->type = VCHR;
	attr->rdev = (uintptr_t)dn - HHDM_BASE;
	return 0;
}

int
dev_spec_read(vnode_t *vn, void *buf, size_t length, off_t offset, int flags)
{
	dev_node_t *dn = VTODN(vn);

	switch (dn->kind) {
	case DEV_KIND_CHAR:
		return dn->ops->read(dn->devprivate, buf, length,
		    offset, flags);

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

	switch (dn->kind) {
	case DEV_KIND_CHAR:
		return dn->ops->write(dn->devprivate, buf, length,
		    offset, flags);

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

	switch (dn->kind) {
	case DEV_KIND_CHAR:
		return dn->ops->ioctl(dn->devprivate, cmd, arg);

	case DEV_KIND_STREAM:
		return strioctl(vn, dn->stdata, cmd, arg);

	default:
		kfatal("implement me");
	}
}

static int
dev_spec_chpoll(vnode_t *vn, struct poll_entry *pe, enum chpoll_mode mode)
{
	dev_node_t *dn = VTODN(vn);

	switch (dn->kind) {
	case DEV_KIND_STREAM:
		return strchpoll(dn->stdata, pe, mode);

	default:
		kfatal("implement me");
	}

}

static int
dev_spec_mmap(void *addr, size_t len, int prot, int flags, vnode_t *vn,
    off_t offset, vaddr_t *window)
{
	dev_node_t *dn = VTODN(vn);

	switch (dn->kind) {
	case DEV_KIND_CHAR:
		return dn->ops->mmap(addr, len, prot, flags,
		    dn->devprivate, offset, window);

	default:
		ktodo();
	}
}

iop_return_t
dev_spec_iop_dispatch(vnode_t *vn, struct iop *iop)
{
	dev_node_t *dn = VTODN(vn);
	return dn->ops->iop_dispatch(dn->devprivate, iop);
}

iop_return_t
dev_spec_iop_complete(vnode_t *vn, struct iop *iop)
{
	dev_node_t *dn = VTODN(vn);
	return dn->ops->iop_complete(dn->devprivate, iop);
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
	.close = dev_spec_close,
	.getattr = dev_spec_getattr,
	.read = dev_spec_read,
	.write = dev_spec_write,
	.ioctl = dev_spec_ioctl,
	.chpoll = dev_spec_chpoll,
	.mmap = dev_spec_mmap,
	.stack_depth = 2,
	.iop_dispatch = dev_spec_iop_dispatch,
	.iop_complete = dev_spec_iop_complete,
};

static struct vnode_ops dev_vnops = {
	.lookup = dev_lookup,
	.getattr = dev_getattr,
};
