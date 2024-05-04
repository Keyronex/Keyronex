#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <stddef.h>
#include <stdint.h>

#include "dev.h"

RB_HEAD(ubc_window_tree, ubc_window);

typedef enum vtype { VNON, VREG, VDIR, VCHR, VLNK, VSOCK, VFIFO } vtype_t;

typedef struct vattr {
	enum vtype type;
} vattr_t;

typedef struct vnode {
	size_t refcnt;
	vtype_t type;
	struct vnode_ops *ops;
	uintptr_t fs_data;
	struct vfs *vfs;

	/*! General rwlock. */
	kmutex_t rwlock;
	/*! Paging I/O rqlock. */
	kmutex_t paging_rwlock;

	/*! UBC windows into this vnode; protected by UBC spinlock */
	struct ubc_window_tree ubc_windows;
	/*! this vnode's VM object */
	vm_object_t *object;
} vnode_t;

typedef struct vfs {
	/*! the filesystem device */
	DKDevice *device;
	struct vfs_ops *ops;
} vfs_t;

struct vnode_ops {
	io_off_t (*readdir)(vnode_t *dvn, void *buf, size_t nbyte,
	    size_t bytes_read, io_off_t seqno);
};

struct vfs_ops {
	vnode_t (*root)(vfs_t *vfs);
};

/*!
 * Allocate a vnode.
 */
vnode_t *vnode_alloc(void);

static inline vnode_t *
vn_retain(vnode_t *vnode)
{
	return vnode;
}
static inline void
vn_release(vnode_t *vnode)
{
}

#endif /* KRX_KDK_VFS_H */
