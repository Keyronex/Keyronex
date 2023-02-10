/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#include <string.h>

#include <libkern/libkern.h>
#include <vfs/vfs.h>

vfs_t	 root_vfs;
vnode_t *dev_vnode = NULL;
vnode_t *root_vnode = NULL;

static int
reduce(vnode_t *parent, vnode_t **vn)
{
	vnode_t *rvn = *vn;

	while (rvn->vfsmountedhere != NULL) {
		vnode_t *root;
		int	 r;

		r = rvn->vfsmountedhere->ops->root(rvn->vfsmountedhere, &root);
		if (r < 0)
			return r;

		// vn_unref(vn); todo
		rvn = root;
	}
	if (rvn->type == VLNK) {
		nk_fatal("handle symlinks in lookup please\n");
	}

	*vn = rvn;
	return 0;
}

int
vfs_lookup(vnode_t *cwd, vnode_t **out, const char *pathname,
    enum lookup_flags flags, vattr_t *attr)
{
	vnode_t *vn, *prevvn = NULL;
	char	 path[255], *sub, *next;
	size_t	 sublen;
	bool	 last = false;
	bool	 mustdir = flags & kLookupMustDir;
	size_t	 len = strlen(pathname);
	int	 r;

	if (pathname[0] == '/' || cwd == NULL) {
		vn = root_vnode;
		if (*(pathname + 1) == '\0') {
			*out = vn;
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
			*out = vn;
			return 0;
		}
	}

loop:
	sublen = 0;
	next = sub;

	while (*next != '\0' && *next != '/') {
		next++;
		sublen++;
	}

	if (*next == '\0') {
		/* end of path */
		last = true;
	} else
		*next = '\0';

	if (strcmp(sub, ".") == 0 || sublen == 0)
		goto next; /* . or trailing */

	prevvn = vn;

	if (!last || !(flags & kLookupCreat)) {
		// kprintf("lookup %s in %p\n", sub, vn);
		r = vn->ops->lookup(vn, &vn, sub);
		if (r == 0)
			r = reduce(prevvn, &vn);
	} else if (flags & kLookupCreat)
		r = vn->ops->create(vn, &vn, sub, attr);


	if (r < 0) {
		// vn_unref(vn);
		return r;
	}

next:
	if (last)
		goto out;

	sub += sublen + 1;
	// vn_unref(prevvn)
	goto loop;

out:
	if (mustdir) {
		r = reduce(prevvn, &vn);
		if (r != 0) {
			// vn_unref(prevvn);
			// vn_unref(vn);
			// return r;
			kfatal("reduced failed\n");
		}
	}

	*out = vn;
	return 0;
}
