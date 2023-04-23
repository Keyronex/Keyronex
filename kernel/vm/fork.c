/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Feb 12 2023.
 */

#include "kdk/amd64/vmamd64.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "machdep/amd64/amd64.h"
#include "vm_internal.h"

/*!
 * Copy entries from one amap to a new empty amap.
 */
int
vmp_amap_copy(vm_map_t *map_from, vm_map_t *map_to, struct vm_amap *from_amap,
    struct vm_amap *to_amap)
{
	vm_page_t *page;
	kwaitstatus_t w;
	int ret;

	w = ke_wait(&from_amap->mutex, "vmp_amap_copy:amap->mutex", false,
	    false, -1);
	kassert(w == kKernWaitStatusOK);

	for (int i = 0; i < elementsof(from_amap->l3->entries); i++) {
		if (from_amap->l3->entries[i] == NULL)
			continue;

		struct vmp_amap_l2 *from_l2 = from_amap->l3->entries[i], *to_l2;

		ret = vmp_page_alloc(map_to, true, kPageUseVMM, &page);
		kassert(ret == 0);

		to_l2 = to_amap->l3->entries[i] = VM_PAGE_DIRECT_MAP_ADDR(page);

		/* copy the l1s of this l2 */
		for (int i = 0; i < elementsof(from_l2->entries); i++) {
			if (from_l2->entries[i] == NULL)
				continue;

			struct vmp_amap_l1 *from_l1 = from_l2->entries[i],
					   *to_l1;

			ret = vmp_page_alloc(map_to, true, kPageUseVMM, &page);
			kassert(ret == 0);

			to_l1 = to_l2->entries[i] = VM_PAGE_DIRECT_MAP_ADDR(
			    page);

			/* increment the anons' refcnts */
			for (int i = 0; i < elementsof(from_l1->entries); i++) {
				if (from_l1->entries[i] == NULL)
					continue;
				__atomic_add_fetch(&from_l1->entries[i]->refcnt,
				    1, __ATOMIC_SEQ_CST);
			}

			/* and copy their pointers */
			memcpy(to_l1, from_l1, sizeof(*from_l1));
		}
	}

	ke_mutex_release(&from_amap->mutex);

	return 0;
}

int
vm_map_fork(vm_map_t *map, vm_map_t **map_out)
{
	vm_map_t *map_new;
	vm_map_entry_t *vad;
	kwaitstatus_t w;

	map_new = kmem_alloc(sizeof(*map_new));
	vm_map_init(map_new);

	if (map == kernel_process.map)
		goto out;

	ps_curthread()->in_pagefault = true;

	w = ke_wait(&map->mutex, "vm_map_fork:map->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	RB_FOREACH (vad, vm_map_entry_rbtree, &map->entry_queue) {
		switch (vad->inheritance) {
		case kVMInheritShared: {
			if (vad->is_phys) {
				vaddr_t vaddr = vad->start;
				int r;
				r = vm_map_phys(map_new, vad->paddr, &vaddr,
				    vad->end - vad->start, vad->protection,
				    vad->max_protection, vad->inheritance,
				    true);
				kassert(r == 0);
			} else {
				vaddr_t vaddr = vad->start;
				int r;

				kassert(!vad->has_anonymous);

				r = vm_map_object(map_new, vad->object, &vaddr,
				    vad->end - vad->start - 1, vad->offset,
				    vad->protection, vad->max_protection,
				    vad->inheritance, true, false);
				kassert(r == 0 && vaddr == vad->start);
			}
			break;
		}
		case kVMInheritCopy: {
			vm_map_entry_t *entry_new;
			vaddr_t vaddr = vad->start;
			int r;

			r = vm_map_object(map_new, vad->object, &vaddr,
			    vad->end - vad->start, vad->offset, vad->protection,
			    vad->max_protection, vad->inheritance, true, true);
			kassert(r == 0 && vaddr == vad->start);

			entry_new = vmp_map_find(map_new, vad->start);
			kassert(entry_new != NULL);

			if (vad->has_anonymous) {
				pmap_protect_range(map, vad->start, vad->end,
				    kVMRead | kVMExecute);
				r = vmp_amap_copy(map, map_new, &vad->amap,
				    &entry_new->amap);
				kassert(r == 0);
			}
			break;
		}
		case kVMInheritStack:
			/* epsilon; handle it explicitly elsewhere */
			break;
		}
	}

	ps_curthread()->in_pagefault = false;

	/* try to force TLB clear */
	vm_map_activate(map);
	pmap_global_invlpg(-1);

	ke_mutex_release(&map->mutex);

out:
	*map_out = map_new;

	return 0;
}

void
vm_map_activate(vm_map_t *map)
{
	kassert((uintptr_t)map >= HHDM_BASE);
	uint64_t val = (uint64_t)map->md.cr3;
	write_cr3(val);
}