/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun May 07 2023.
 */

#include <sys/errno.h>

#include <dirent.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/process.h>
#include <kdk/vfs.h>
#include <linux/magic.h>

#include "kdk/kernel.h"
#include "kdk/object.h"

#define VNTOPN(VN) ((procnode_t *)(VN)->data)

enum procfs_node_kind {
	kPNRoot,
	kPNMounts,
};

struct procfs_target {
	const char *name;
} targets[] = { [kPNRoot] = {
		    "mounts",
		} };

typedef struct procnode {
	enum procfs_node_kind kind;
	vnode_t *vn;
} procnode_t;

struct vfsops proc_vfsops;
struct vnops proc_vnops;
static vfs_t *proc_vfs;
static procnode_t *mounts_node = NULL;

static vtype_t
pn_vtype(enum procfs_node_kind kind)
{
	switch (kind) {
	case kPNRoot:
		return VDIR;

	case kPNMounts:
		return VREG;

	default:
		kfatal("unreached");
	}
}

static int
proc_makevnode(procnode_t *pn, vnode_t **out)
{
	vnode_t *vn;

	vn = kmem_alloc(sizeof(*vn));
	pn->vn = vn;

	obj_initialise_header(&vn->objhdr, kObjTypeVNode);
	ke_mutex_init(&vn->lock);
	vn->locked_for_paging = false;
	vn->type = pn_vtype(pn->kind);
	vn->ops = &proc_vnops;
	vn->vfsp = proc_vfs;
	vn->vfsmountedhere = NULL;
	vn->path = NULL;

	if (pn->kind == kPNRoot)
		vn->isroot = true;
	else
		vn->isroot = false;

	vn->vmobj = NULL;

	vn->data = (uintptr_t)pn;

	*out = vn;

	return 0;
}

static int
proc_getvnode(procnode_t *pn, vnode_t **out)
{
	if (pn->vn != NULL) {
		*out = obj_direct_retain(pn->vn);
		return 0;
	} else {
		vnode_t *vn;
		int r;
		r = proc_makevnode(pn, &vn);
		if (r == 0) {
			obj_direct_retain(vn);
		}
		*out = vn;
		return r;
	}
}

static procnode_t *
proc_getnode(procnode_t *folder, const char *name)
{
	kassert(strcmp(name, "mounts") == 0);
	if (!mounts_node) {
		mounts_node = kmem_alloc(sizeof(procnode_t));
		mounts_node->kind = kPNMounts;
		mounts_node->vn = NULL;
	}

	return mounts_node;
}

static int
procfs_mount(vfs_t *vfs, vnode_t *over, void *data)
{
	procnode_t *root = kmem_alloc(sizeof(*root));
	root->vn = NULL;

	root->kind = kPNRoot;
	root->vn = NULL;

	vfs->ops = &proc_vfsops;
	vfs->data = (uintptr_t)root;
	vfs->devname = "proc";
	vfs->mountpoint = "/proc";
	vfs->type = "procfs";

	proc_vfs = vfs;

	/* todo: lock mount lock */
	over->vfsmountedhere = vfs;
	TAILQ_INSERT_TAIL(&vfs_tailq, vfs, tailq_entry);

	return 0;
}

static int
procfs_root(vfs_t *vfs, vnode_t **out)
{
	procnode_t *root = (procnode_t *)vfs->data;
	return proc_getvnode(root, out);
}

static int
procfs_statfs(vfs_t *vfs, struct statfs *out)
{
	out->f_type = S_MAGIC_PROC;
	out->f_bsize = PGSIZE;
	out->f_blocks = 0;
	out->f_bfree = 0;
	out->f_bavail = 0;

	out->f_files = 0;
	out->f_ffree = 0;
	out->f_fsid.__val[0] = 0;
	out->f_fsid.__val[1] = 0;
	out->f_namelen = 255;
	out->f_frsize = 0;
	out->f_flags = 0;

	return 0;
}

static int
proc_read(vnode_t *vn, void *buf, size_t nbyte, off_t off, int flags)
{
	procnode_t *pn = VNTOPN(vn);
	vfs_t *vfs;
	ipl_t ipl;
	char *text = NULL;

	kassert(pn->kind == kPNMounts);

	ipl = ke_spinlock_acquire(&mount_lock);
	TAILQ_FOREACH (vfs, &vfs_tailq, tailq_entry) {
		char *newtext;
		kmem_asprintf(&newtext, "%s%s %s %s %s %d %d\n",
		    text == NULL ? "" : text, vfs->devname, vfs->mountpoint,
		    vfs->type, "rw", 0, 0);
		if (text != NULL)
			kmem_strfree(text);
		text = newtext;
	}
	ke_spinlock_release(&mount_lock, ipl);

	int n_to_read = MIN2(strlen(text) - off, nbyte);
	if (n_to_read < 0)
		n_to_read = 0;
	if (n_to_read > 0)
		memcpy(buf, text + off, n_to_read);

	kmem_strfree(text);

	return n_to_read;
}

static int
proc_getattr(vnode_t *vn, vattr_t *out)
{
	procnode_t *pn = VNTOPN(vn);
	memset(out, 0x0, sizeof(*out));
	out->type = pn_vtype(pn->kind);
	out->mode = out->type == VDIR ? 0755 : 0644;
	return 0;
}

static int
proc_lookup(vnode_t *vn, vnode_t **out, const char *name)
{
	procnode_t *pdn = VNTOPN(vn);

	if (strcmp(name, ".") == 0) {
		*out = obj_direct_retain(vn);
		return 0;
	} else if (strcmp(name, "..") == 0) {
		*out = obj_direct_retain(vn);
		return 0;
	}

	for (int i = 0; i < elementsof(targets); i++) {
		procnode_t *pn;

		if (strcmp(targets[i].name, name) != 0)
			continue;

		pn = proc_getnode(pdn, name);

		return proc_getvnode(pn, out);
	}

	return -ENOENT;
}

static off_t
proc_readdir(vnode_t *dvn, void *buf, size_t nbyte, size_t *bytesRead,
    off_t seqno)
{
	struct dirent *dentp = buf;
	size_t nwritten = 0;
	size_t i;

	for (i = 0;; i++) {
		if (i >= seqno) {
			size_t reclen;
			const char *name;
			ino_t ino;
			unsigned char type = DT_UNKNOWN;

			if (i == 0) {
				name = ".";
				ino = 2;
			} else if (i == 1) {
				name = "..";
				ino = (ino_t)2;
			} else {
				struct procfs_target *target;

				if (i - 2 >= elementsof(targets))
					goto finish;

				target = &targets[i - 2];
				name = target->name;
			}

			reclen = DIRENT_RECLEN(strlen(name));

			if ((void *)dentp + reclen > buf + nbyte - 1) {
				goto finish;
			}

			dentp->d_ino = ino;
			dentp->d_off = i;
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

struct vfsops proc_vfsops = {
	.mount = procfs_mount,
	.root = procfs_root,
	.statfs = procfs_statfs,
};

struct vnops proc_vnops = {
	.read = proc_read,
	.getattr = proc_getattr,

	.lookup = proc_lookup,

	.readdir = proc_readdir,
};
