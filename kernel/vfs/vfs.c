
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/vfs.h"

obj_class_t vnode_class;

vnode_t *
vnode_alloc(void)
{
	vnode_t *vnode;
	obj_new(&vnode, vnode_class, sizeof(vnode_t), NULL);
	return vnode;
}

vnode_t *
vnode_new(vfs_t *vfs, vtype_t type, struct vnode_ops *ops, kmutex_t *rwlock,
    kmutex_t *paging_rwlock, uintptr_t fs_data)
{
	vnode_t *vnode = vnode_alloc();

	vnode->type = type;
	vnode->fs_data = fs_data;
	vnode->ops = ops;
	vnode->vfs = vfs;
	vnode->rwlock = rwlock;
	vnode->paging_rwlock = paging_rwlock;

	RB_INIT(&vnode->ubc_windows);
}
