/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon May 08 2023.
 */

#include <kdk/kmem.h>
#include <kdk/vfs.h>
#include <kdk/vm.h>

int
vm_object_new_vnode(vm_object_t **out, struct vnode *vnode)
{
	vm_object_t *obj = kmem_alloc(sizeof(*obj));

	obj_initialise_header(&obj->objhdr, kObjTypeSection);
	ke_mutex_init(&obj->mutex);

	obj->is_anonymous = false;
	RB_INIT(&obj->page_rbtree);
	obj->vnode = vnode;

	*out = obj;
	return 0;
}
