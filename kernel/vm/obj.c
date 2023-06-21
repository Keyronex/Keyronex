/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon May 08 2023.
 */

#include <kdk/kmem.h>
#include <kdk/process.h>
#include <kdk/vfs.h>
#include <kdk/vm.h>

#include "vm/vm_internal.h"

int
vm_object_new_vnode(vm_object_t **out, struct vnode *vnode)
{
	vm_object_t *obj = kmem_alloc(sizeof(*obj));

	obj_initialise_header(&obj->objhdr, kObjTypeSection);
	ke_mutex_init(&obj->mutex);

	obj->is_anonymous = false;
	obj->ndirty = 0;
	RB_INIT(&obj->page_rbtree);
	obj->vnode = vnode;

	*out = obj;
	return 0;
}

int
vm_object_free(vm_map_t *map, vm_object_t *obj)
{
	/* here, we know the object can't be mapped anywhere anymore */

	if (obj->is_anonymous)
		vmp_amap_free(map, &obj->amap);
	else {
		struct vmp_objpage *objpage;
		RB_FOREACH(objpage, vmp_objpage_rbtree ,&obj->page_rbtree ) {
			/* todo(HIGH): !!! Come back to this ; queues? */
			vmp_page_free(map, objpage->page);
			objpage->page = NULL;
		}
		kmem_free(obj, sizeof(*obj));
	}
	return 0;
}
