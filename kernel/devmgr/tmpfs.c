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
extern struct vfsops tmpfs_vfsops;

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
		obj_retain(&node->vn->objhdr);
		*vout = node->vn;
		return 0;
	} else {
		vnode_t *vn = kmem_alloc(sizeof(*vn));
		node->vn = vn;
		obj_initialise_header(&vn->objhdr, kObjTypeVNode);
		vn->type = node->attr.type;
		vn->ops = vn->type == VCHR ? &tmpfs_spec_vnops : &tmpfs_vnops;
		vn->vfsp = vfs;
		vn->vfsmountedhere = NULL;
		vn->isroot = false;
		if (node->attr.type == VREG) {
			vn->section = node->reg.section;
		} else if (node->attr.type == VCHR) {
			kfatal("Unimplemented\n");
			// spec_setup_vnode(vn, node->attr.rdev);
		}
		vn->data = (uintptr_t)node;
		*vout = vn;
		return 0;
	}
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
		int r;
		r = vm_section_new_anonymous(&kernel_process.vmps, UINT32_MAX,
		    &n->reg.section);
		kassert(r == 0);
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
	vaddr_t vaddr = -1;
	tmpnode_t *tn = VNTOTN(vn);
	int r;

	if (tn->attr.type != VREG)
		return -EINVAL;

	if (off + nbyte > tn->attr.size)
		nbyte = tn->attr.size <= off ? 0 : tn->attr.size - off;
	if (nbyte == 0)
		return 0;

	r = vm_ps_map_section_view(&kernel_process.vmps, vn->section, &vaddr,
	    PGROUNDUP(nbyte + off), 0x0, kVMRead, kVMRead, kVADInheritShared,
	    false);
	kassert(r == 0);

	memcpy(buf, (void *)(vaddr + off), nbyte);

	r = vm_ps_deallocate(&kernel_process.vmps, vaddr,
	    PGROUNDUP(nbyte + off));
	kassert(r == 0);

	return nbyte; /* FIXME */
}

int
tmp_write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	vaddr_t vaddr = -1;
	tmpnode_t *tn = VNTOTN(vn);
	int r;

	if (nbyte == 0)
		return 0;

	if (off + nbyte > tn->attr.size)
		tn->attr.size = off + nbyte;

	r = vm_ps_map_section_view(&kernel_process.vmps, vn->section, &vaddr,
	    PGROUNDUP(nbyte + off), 0x0, kVMAll, kVMAll, kVADInheritShared,
	    false);
	kassert(r == 0);

	memcpy((void *)(vaddr + off), buf, nbyte);

	r = vm_ps_deallocate(&kernel_process.vmps, vaddr,
	    PGROUNDUP(nbyte + off));

	return nbyte;
}

#define DIRENT_RECLEN(NAMELEN) \
	ROUNDUP(offsetof(struct dirent, d_name[0]) + 1 + NAMELEN, 8)

int
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
		if (!tdent) {
			i = INT32_MAX;
			goto finish;
		}

		if (i >= seqno) {
			size_t reclen = DIRENT_RECLEN(strlen(tdent->name));

			if ((void *)dentp + reclen > buf + nbyte - 1) {
				i--;
				goto finish;
			}

			dentp->d_ino = (uint64_t)tdent->node;
			dentp->d_off = i++;
			dentp->d_reclen = reclen;
			dentp->d_type = DT_UNKNOWN;
			strcpy(dentp->d_name, tdent->name);

			nwritten += reclen;
			dentp = (void *)dentp + reclen;
		}

		tdent = TAILQ_NEXT(tdent, entries);
	}

finish:
	*bytesRead = nwritten;
	return i;
}

struct vfsops tmpfs_vfsops = {
	.root = tmpfs_root,
	.vget = tmpfs_vget,
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
	.getattr = tmp_getattr,
#if 0
	.open = spec_open,
	.read = spec_read,
	.write = spec_write,
#endif
};