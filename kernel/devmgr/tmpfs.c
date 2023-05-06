/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Mar 03 2023.
 */

#include "abi-bits/errno.h"
#include "dirent.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "tmpfs.h"

vfs_t dev_vfs;
vnode_t *dev_vnode;

extern struct vnops tmpfs_vnops;
extern struct vnops tmpfs_spec_vnops;

int
devfs_setup_vnode(vnode_t *vn, struct device *rdevice, struct vnops *devvnops)
{
	vn->ops = &tmpfs_spec_vnops;
	vn->rdevice = rdevice;
	vn->rdeviceops = devvnops;
	return 0;
}

static int
devfs_close(vnode_t *vn)
{
	if (vn->rdeviceops->close != NULL)
		return vn->rdeviceops->close(vn);
	return 0;
}

static int
devfs_ioctl(vnode_t *vn, unsigned long command, void *arg)
{
	return vn->rdeviceops->ioctl(vn, command, arg);
}

static int
devfs_read(vnode_t *vn, void *buf, size_t size, off_t off)
{
	return vn->rdeviceops->read(vn, buf, size, off);
}

static int
devfs_write(vnode_t *vn, void *buf, size_t size, off_t off)
{
	return vn->rdeviceops->write(vn, buf, size, off);
}

static int
devfs_chpoll(vnode_t *vn, struct pollhead *ph, enum chpoll_kind kind)
{
	return vn->rdeviceops->chpoll(vn, ph, kind);
}

static int
devfs_mmap(vnode_t *vn, vm_map_t *map, krx_inout vaddr_t *vaddrp, size_t size,
    voff_t offset, vm_protection_t initial_protection,
    vm_protection_t max_protection, enum vm_inheritance inheritance, bool exact,
    bool copy)
{
	return vn->rdeviceops->mmap(vn, map, vaddrp, size, offset,
	    initial_protection, max_protection, inheritance, exact, copy);
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
		obj_direct_retain(node->vn);
		*vout = node->vn;
		return 0;
	} else {
		vnode_t *vn = kmem_alloc(sizeof(*vn));
		node->vn = vn;
		obj_initialise_header(&vn->vmobj.objhdr, kObjTypeVNode);
		vn->type = node->attr.type;
		vn->ops = vn->type == VCHR ? &tmpfs_spec_vnops : &tmpfs_vnops;
		vn->vfsp = vfs;
		vn->vfsmountedhere = NULL;
		/* root vnode is kept referenced */
		vn->isroot = false;
		if (node->attr.type == VREG) {
			kfatal(
			    "TmpFS regular nodes must always have their vnode.\n");
		} else if (node->attr.type == VCHR) {
			devfs_setup_vnode(vn, node->attr.rdevice,
			    node->attr.rdevops);
		}
		vn->data = (uintptr_t)node;
		*vout = vn;
		return 0;
	}
}

int
tmpfs_mount(vfs_t *vfs, vnode_t *over, void *data)
{
	tmpnode_t *root = kmem_alloc(sizeof(*root));
	vnode_t *vroot;

	memset(&root->attr, 0x0, sizeof(root->attr));
	root->attr.mode = 0755;
	root->attr.type = VDIR;
	root->vn = NULL;
	TAILQ_INIT(&root->dir.entries);

	tmpfs_vget(vfs, &vroot, (ino_t)root);
	vroot->isroot = true;
	vfs->ops = &tmpfs_vfsops;
	vfs->data = (uintptr_t)vroot;

	over->vfsmountedhere = vfs;

	return 0;
}

/*! Special mount procedure used for initial devfs. */
int
vfs_mountdev1(void)
{
	tmpnode_t *root = kmem_alloc(sizeof(*root));
	vnode_t *vroot;

	root->attr.type = VDIR;
	root->vn = NULL;
	TAILQ_INIT(&root->dir.entries);

	tmpfs_vget(&dev_vfs, &vroot, (ino_t)root);
	vroot->isroot = true;
	dev_vfs.ops = &tmpfs_vfsops;
	dev_vfs.data = (uintptr_t)vroot;
	dev_vnode = vroot;

	return 0;
}

/*
 * vnops
 */

#define VNTOTN(VN) ((tmpnode_t *)VN->data)

static tmpdirent_t *
tlookup(tmpnode_t *node, const char *filename)
{
	tmpdirent_t *dent;
	TAILQ_FOREACH (dent, &node->dir.entries, entries) {
		kassert(filename && dent && dent->name);
		if (strcmp(dent->name, filename) == 0)
			return dent;
	}
	return NULL;
}

static tmpnode_t *
tmakenode(tmpnode_t *dn, const char *name, vattr_t *attr)
{
	tmpnode_t *n = kmem_alloc(sizeof(*n));
	tmpdirent_t *td = kmem_alloc(sizeof(*td));

	td->name = strdup(name);
	td->node = n;

	if (attr) {
		n->attr = *attr;
	} else {
		memset(&n->attr, 0x0, sizeof(n->attr));
	}

	n->attr.size = 0;
	n->vn = NULL;

	switch (attr->type) {
	case VREG: {
		// int r;
		//  r = vm_object_new_anonymous(kernel_process.map, UINT32_MAX,
		//       &n->reg.section);
		// kassert(r == 0);
		kfatal("Implement\n");
		break;
	}
	case VDIR:
		TAILQ_INIT(&n->dir.entries);
		n->dir.parent = dn;
		break;

	case VCHR:
		/* epsilon */
		break;

	default:
		kassert("unreached");
	}

	TAILQ_INSERT_TAIL(&dn->dir.entries, td, entries);

	return n;
}

static int
tmp_create(vnode_t *dvn, vnode_t **out, const char *pathname, vattr_t *attr)
{
	tmpnode_t *n;

	kassert(dvn->type == VDIR);

	n = tmakenode(VNTOTN(dvn), pathname, attr);
	kassert(n != NULL);

	kassert(dvn && dvn->vfsp && dvn->vfsp->ops && dvn->vfsp->ops->vget);
	return dvn->vfsp->ops->vget(dvn->vfsp, out, (ino_t)n);
}

int
tmp_lookup(vnode_t *vn, vnode_t **out, const char *pathname)
{
	tmpnode_t *node = VNTOTN(vn);
	tmpdirent_t *tdent;
	int r;

	kassert(node->attr.type == VDIR);

	if (strcmp(pathname, "..") == 0) {
		if (!node->dir.parent)
			*out = vn;
		else
			*out = node->dir.parent->vn;
		return 0;
	}

	tdent = tlookup(node, pathname);
	if (!tdent)
		return -ENOENT;

	r = vn->vfsp->ops->vget(vn->vfsp, out, (ino_t)tdent->node);

	return r;
}

int
tmp_getattr(vnode_t *vn, vattr_t *out)
{
	tmpnode_t *tn = VNTOTN(vn);
	*out = tn->attr;
	return 0;
}

int
tmp_read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_read(vn, buf, nbyte, off);
}

int
tmp_write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_write(vn, buf, nbyte, off);
}

off_t
tmp_readdir(vnode_t *dvn, void *buf, size_t nbyte, size_t *bytesRead,
    off_t seqno)
{
	tmpnode_t *n = VNTOTN(dvn);
	tmpdirent_t *tdent;
	struct dirent *dentp = buf;
	size_t nwritten = 0;
	size_t i;

	kassert(n->attr.type == VDIR);

	tdent = TAILQ_FIRST(&n->dir.entries);

	for (i = 0;; i++) {
		if (i >= seqno) {
			size_t reclen;
			const char *name;
			ino_t ino;
			unsigned char type = DT_UNKNOWN;

			if (i == 0) {
				name = ".";
				ino = (ino_t)n;
			} else if (i == 1) {
				name = "..";
				ino = (ino_t)(n->dir.parent == NULL ?
					n :
					n->dir.parent);
			} else {
				if (!tdent) {
					i--;
					goto finish;
				}
				name = tdent->name;
				ino = (ino_t)tdent->node;
				tdent = TAILQ_NEXT(tdent, entries);
			}

			reclen = DIRENT_RECLEN(strlen(name));

			if ((void *)dentp + reclen > buf + nbyte - 1) {
				i--;
				goto finish;
			}

			dentp->d_ino = ino;
			dentp->d_off = i++;
			dentp->d_reclen = reclen;
			dentp->d_type = type;
			strcpy(dentp->d_name, name);

			nwritten += reclen;
			dentp = (void *)dentp + reclen;
		}
	}

finish:
	*bytesRead = nwritten;
	return i;
}

struct vfsops tmpfs_vfsops = {
	.root = tmpfs_root,
	.vget = tmpfs_vget,
	.mount = tmpfs_mount,
};

struct vnops tmpfs_vnops = {
	.create = tmp_create,
	.lookup = tmp_lookup,
	.getattr = tmp_getattr,
	.read = tmp_read,
	.write = tmp_write,
	.readdir = tmp_readdir,
};

struct vnops tmpfs_spec_vnops = {
	.close = devfs_close,
	.ioctl = devfs_ioctl,
	.getattr = tmp_getattr,
	.read = devfs_read,
	.write = devfs_write,
	.chpoll = devfs_chpoll,
	.mmap = devfs_mmap,
#if 0
	.open = spec_open,
	.read = spec_read,
	.write = spec_write,
#endif
};
