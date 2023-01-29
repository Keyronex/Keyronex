/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#include <dev/dev.h>
#include <kern/kmem.h>
#include <libkern/libkern.h>

#include <errno.h>
#include <vfs/vfs.h>

#include "kern/obj.h"
#include "tmpfs.h"

static int tmpfs_vget(vfs_t *vfs, vnode_t **vout, ino_t ino);

static int
tmpfs_mount(vfs_t *vfs, const char *path, void *data)
{
	tmpnode_t *root = kmem_alloc(sizeof(*root));
	vnode_t	  *vroot;

	root->attr.type = VDIR;
	root->vn = NULL;
	TAILQ_INIT(&root->dir.entries);

	tmpfs_vget(vfs, &vroot, (ino_t)root);
	vroot->isroot = true;
	vfs->data = vroot;

	return 0;
}

static int
tmpfs_root(vfs_t *vfs, vnode_t **out)
{
	*out = (vnode_t *)vfs->data;
	return 0;
}

static int
tmpfs_vget(vfs_t *vfs, vnode_t **vout, ino_t ino)
{
	tmpnode_t *node = (tmpnode_t *)ino;

	if (node->vn != NULL) {
		obj_retain(&node->vn->hdr);
		*vout = node->vn;
		return 0;
	} else {
		vnode_t *vn = kmem_alloc(sizeof(*vn));
		node->vn = vn;
		obj_init(&vn->hdr, kOTVNode);
		vn->type = node->attr.type;
		vn->ops = vn->type == VCHR ? &tmpfs_spec_vnops : &tmpfs_vnops;
		vn->vfsp = vfs;
		vn->vfsmountedhere = NULL;
		vn->isroot = false;
		if (node->attr.type == VREG) {
			vn->vmobj = node->reg.vmobj;
		} else if (node->attr.type == VCHR) {
			spec_setup_vnode(vn, node->attr.rdev);
		}
		vn->data = node;
		*vout = vn;
		return 0;
	}
}

struct vfsops tmpfs_vfsops = {
	.mount = tmpfs_mount,
	.root = tmpfs_root,
	.vget = tmpfs_vget,
};
