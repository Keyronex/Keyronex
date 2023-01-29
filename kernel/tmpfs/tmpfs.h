/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

#ifndef TMPFS_H_
#define TMPFS_H_

#include <vfs/vfs.h>

typedef struct tmpdirent {
	TAILQ_ENTRY(tmpdirent) entries;

	char	       *name;
	struct tmpnode *node;
} tmpdirent_t;

typedef struct tmpnode {
	/** Associated vnode; may be null. It shares its vmobj with this. */
	vnode_t *vn;

	vattr_t attr;

	union {
		/* VDIR case */
		struct {
			TAILQ_HEAD(, tmpdirent) entries;
			struct tmpnode *parent;
		} dir;

		/* VREG case */
		struct {
			vm_object_t *vmobj;
		} reg;
	};
} tmpnode_t;

extern struct vfsops tmpfs_vfsops;
extern struct vnops  tmpfs_vnops;
extern struct vnops  tmpfs_spec_vnops;

#endif /* TMPFS_H_ */
