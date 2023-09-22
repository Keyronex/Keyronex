
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
