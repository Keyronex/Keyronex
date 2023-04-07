/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 02 2023.
 */

#include <sys/stat.h>

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"

vnode_t *root_vnode = NULL;

/*! this works as long as our defs align with Linux */
enum vtype
mode_to_vtype(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return VDIR;

	case S_IFCHR:
		return VCHR;

	case S_IFBLK:
		return VNON;

	case S_IFREG:
		return VREG;

	case S_IFIFO:
		return VNON;

	case S_IFLNK:
		return VLNK;

	case S_IFSOCK:
		return VNON;

	default:
		return VNON;
	}
}

static int
reduce(vnode_t *parent, vnode_t **vn)
{
	vnode_t *rvn;

	kassert(*vn != NULL);
	rvn = obj_direct_retain(*vn);

start:
	while (rvn->vfsmountedhere != NULL) {
		vnode_t *root;
		int r;

		r = rvn->vfsmountedhere->ops->root(rvn->vfsmountedhere, &root);
		if (r < 0)
			return r;

		obj_direct_release(rvn);
		rvn = root;
	}
	if (rvn->type == VLNK) {
		char *buf = kmem_alloc(256);
		vnode_t *target;
		int r;

		r = rvn->ops->readlink(rvn, buf);
		if (r != 0) {
			obj_direct_release(rvn);
			return r;
		}

		r = vfs_lookup(parent, &target, buf, 0, NULL);
		if (r != 0) {
			obj_direct_release(rvn);
			return r;
		}

		obj_direct_release(rvn);
		rvn = target;

		goto start;
	}

	*vn = rvn;
	return 0;
}

int
vfs_lookup(vnode_t *cwd, vnode_t **out, const char *pathname,
    enum lookup_flags flags, vattr_t *attr)
{
	vnode_t *vn, *prevvn = NULL;
	char path[255], *sub, *next;
	size_t sublen;
	bool last = false;
	bool mustdir = flags & kLookupMustDir;
	size_t len = strlen(pathname);
	int r;

#if 0
	kdprintf("VFS_LOOOKUP %s\n", pathname);
#endif

	if (pathname[0] == '/' || cwd == NULL) {
		vn = root_vnode;
		if (*(pathname + 1) == '\0') {
			*out = obj_direct_retain(vn);
			return 0;
		}
	} else
		vn = cwd;

	strcpy(path, pathname);
	sub = path;

	if (path[len - 1] == '/') {
		size_t last = len - 1;
		while (path[last] == '/')
			path[last--] = '\0';
		mustdir = true;
		if (*path == '\0') {
			*out = obj_direct_retain(vn);
			return 0;
		}
	}

	obj_direct_retain(vn);

loop:
	sublen = 0;
	next = sub;

	while (*next != '\0' && *next != '/') {
		next++;
		sublen++;
	}

	if (*next == '\0') {
		if (flags & kLookup2ndLast)
			goto out;
		/* end of path */
		last = true;
	} else
		*next = '\0';

	if (strcmp(sub, ".") == 0 || sublen == 0)
		goto next; /* . or trailing */

	if (!last || !(flags & kLookupCreate)) {
		vnode_t *new_vn = NULL;
		r = vn->ops->lookup(vn, &new_vn, sub);
		if (r == 0) {
			r = reduce(vn, &new_vn);
			if (r == 0) {
				obj_direct_release(vn);
				vn = new_vn;
			}
		}
	} else if (flags & kLookupCreate) {
		vnode_t *new_vn;
		r = vn->ops->create(vn, &new_vn, sub, attr);
		if (r == 0)
			vn = new_vn;
	}

	if (r < 0) {
		obj_direct_release(vn);
		return r;
	}

next:
	if (last)
		goto out;

	sub += sublen + 1;
	if (prevvn)
		obj_direct_release(prevvn);
	prevvn = vn;
	goto loop;

out:
	if (mustdir) {
		kassert(prevvn != NULL);
		r = reduce(prevvn, &vn);
		if (r != 0) {
			obj_direct_release(prevvn);
			obj_direct_release(vn);
			return r;
		}
	}

	if (flags & kLookup2ndLast) {
		if (prevvn == NULL)
			*out = vn;
		else if (vn != NULL) {
			*out = prevvn;
			obj_direct_release(vn);
		} else {
			kfatal("Unreached\n");
		}
		return 0;
	} else {
		if (prevvn)
			obj_direct_release(prevvn);

		*out = vn;
		return 0;
	}
}