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

#include "vfs.h"

vfs_t root_vfs;
vnode_t *dev_vnode = NULL;
vnode_t *root_vnode = NULL;

static vnode_t *
reduce(vnode_t *vn)
{
	/* todo sym-links */
	return vn;
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

	/* reduce here? */

	if (strcmp(sub, ".") == 0 || sublen == 0)
		goto next; /* . or trailing */

	prevvn = vn;

	if (!last || !(flags & kLookupCreat))
		// kprintf("lookup %s in %p\n", sub, vn);
		r = vn->ops->lookup(vn, &vn, sub);
	else if (flags & kLookupCreat)
		r = vn->ops->create(vn, &vn, sub, attr);

	if (prevvn != vn)
		// vn_unref(vn); TODO:
		;

	if (r < 0) {
		// vn_unref(vn);
		return r;
	}

	while (vn->vfsmountedhere != NULL) {
		vnode_t *root;
                int r;

		r = vn->vfsmountedhere->ops->root(vn->vfsmountedhere, &root);
                if (r < 0) {
                        // vn_unref(vn);
                        return r;
                }
                // vn_unref(vn); todo
                vn = root;
	}

next:
	if (last)
		goto out;

	sub += sublen + 1;
	goto loop;

out:
	if (mustdir)
		vn = reduce(vn);
	*out = vn;
	return 0;
}
