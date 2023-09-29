#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <stddef.h>
#include <stdint.h>

typedef enum vtype { VNON, VREG, VDIR, VCHR, VLNK, VSOCK, VFIFO } vtype_t;

typedef struct vnode {
	size_t refcnt;
	vtype_t type;
	struct vnode_ops *ops;
	uintptr_t fs_data;
}vnode_t;

struct vnode_ops { };

/*!
 * Allocate a vnode.
 */
vnode_t *vnode_alloc(void);

vnode_t *vn_retain(vnode_t *vnode);
void vn_release(vnode_t *vnode);

#endif /* KRX_KDK_VFS_H */
