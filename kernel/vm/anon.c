/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 29 2023.
 */

#include "kdk/amd64/vmamd64.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "vm/vm_internal.h"

int
vmp_anon_release(vm_map_t *map, struct vmp_anon *anon)
{
	/* TODO(swap): think about how whether this may race with pagedaemon */
	if (--anon->refcnt == 0) {
		kassert(LIST_EMPTY(&anon->page->pv_list));
		if (anon->resident) {
			vmp_page_free(map, anon->page);
		} else {
			kfatal("Unhandled\n");
		}
	}

	kmem_free(anon, sizeof(*anon));
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

		/* copy the l1s of this l2 */
		for (int i = 0; i < elementsof(l2->entries); i++) {
			if (l2->entries[i] == NULL)
				continue;

			struct vmp_amap_l1 *l1 = l2->entries[i];
			vm_page_t *l1_page = vmp_paddr_to_page(
			    (paddr_t)V2P(l1));

			/* increment the anons' refcnts */
			for (int i = 0; i < elementsof(l1->entries); i++) {
				if (l1->entries[i] == NULL)
					continue;
				vmp_anon_release(map, l1->entries[i]);
				l1->entries[i] = (void *)0xDEADBEEF;
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