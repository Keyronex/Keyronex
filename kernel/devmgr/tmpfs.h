/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Mar 03 2023.
 */

#ifndef KRX_DEVMGR_TMPFS_H
#define KRX_DEVMGR_TMPFS_H

#include "kdk/vfs.h"

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
			vm_section_t *section;
		} reg;
	};
} tmpnode_t;

#endif /* KRX_DEVMGR_TMPFS_H */
