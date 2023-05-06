/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 29 2023.
 */

#include "kdk/amd64/vmamd64.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/vm.h"
#include "vm/vm_internal.h"

int
vmp_anon_release(vm_map_t *map, struct vmp_anon *anon, bool mutex_held)
{
	/* TODO(swap): think about how whether this may race with pagedaemon */
	if (--anon->refcnt == 0) {
		kassert(LIST_EMPTY(&anon->page->pv_list));
		if (anon->resident) {
			vmp_page_free(map, anon->page);
		} else {
			kfatal("Unhandled\n");
		}
		if (mutex_held)
			ke_mutex_release(&anon->mutex);
		kmem_free(anon, sizeof(*anon));
		return 0;
	}

	if (mutex_held)
		ke_mutex_release(&anon->mutex);

	return 0;
}

int
vmp_amap_free(vm_map_t *map, struct vm_amap *amap)
{
	kwaitstatus_t w;

	w = ke_wait(&amap->mutex, "vmp_amap_free:amap->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	if (!amap->l3)
		goto conclude;

	for (size_t i = 0; i < elementsof(amap->l3->entries); i++) {
		if (amap->l3->entries[i] == NULL)
			continue;

		struct vmp_amap_l2 *l2 = amap->l3->entries[i];
		vm_page_t *l2_page = vmp_paddr_to_page((paddr_t)V2P(l2));

		for (int i2 = 0; i2 < elementsof(l2->entries); i2++) {
			if (l2->entries[i2] == NULL)
				continue;

			struct vmp_amap_l1 *l1 = l2->entries[i2];
			vm_page_t *l1_page = vmp_paddr_to_page(
			    (paddr_t)V2P(l1));

			for (int i3 = 0; i3 < elementsof(l1->entries); i3++) {
				if (l1->entries[i3] == NULL)
					continue;
				vmp_anon_release(map, l1->entries[i3], false);
				l1->entries[i3] = (void *)0xDEADBEEF;
			}

			vm_page_unwire(l1_page);
			vmp_page_free(map, l1_page);
		}

		vm_page_unwire(l2_page);
		vmp_page_free(map, l2_page);
	}

	vm_page_t *l3_page = vmp_paddr_to_page((paddr_t)V2P(amap->l3));

	vm_page_unwire(l3_page);
	vmp_page_free(map, l3_page);

	ke_mutex_release(&amap->mutex);

conclude:
	return 0;
}

int vm_object_new_anonymous(vm_map_t *map, size_t size, vm_object_t **out)
{
	vm_object_t *obj = kmem_alloc(sizeof(*obj));
	int r;

	obj_initialise_header(&obj->objhdr, kObjTypeSection);
	ke_mutex_init(&obj->mutex);
	obj->is_anonymous = true;
	r = vmp_amap_init(map, &obj->amap);
	kassert(r == 0);

	*out = obj;
	return r;
}
