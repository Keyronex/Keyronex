/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Mar 03 2023.
 */

#ifndef KRX_DEVMGR_TMPFS_H
#define KRX_DEVMGR_TMPFS_H

#include "kdk/vfs.h"

typedef struct tmpdirent {
	TAILQ_ENTRY(tmpdirent) entries;

	char *name;
	struct tmpnode *node;
} tmpdirent_t;

typedef struct tmpnode {
	size_t linkcnt; /* number of links to the node */
	vnode_t *vn;	/* associated vnode; permanent if VREG. */
	vattr_t attr; /* file attributes */

	union {
		/* VDIR case */
		struct {
			TAILQ_HEAD(, tmpdirent) entries;
			struct tmpnode *parent;
			unsigned is_root : 1;
		} dir;
	};
} tmpnode_t;

extern struct vfsops tmpfs_vfsops;

#endif /* KRX_DEVMGR_TMPFS_H */
