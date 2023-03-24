/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 17 2023.
 */

#include "kdk/amd64/vmamd64.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "vm/vm_internal.h"

RB_GENERATE(vm_map_entry_rbtree, vm_map_entry, rbtree_entry, vmp_map_entry_cmp);

int
vmp_map_entry_cmp(vm_map_entry_t *x, vm_map_entry_t *y)
{
	/*
	 * what this actually does is determine whether x's start address is
	 * lower than, greater than, or within the bounds of Y. it works because
	 * we allocate virtual address space with vmem, which already ensures
	 * there are no overlaps.
	 */

	if (x->start < y->start)
		return -1;
	else if (x->start >= y->end)
		return 1;
	else
		/* x->start is within VAD y */
		return 0;
}

vm_map_entry_t *
vmp_map_find(vm_map_t *ps, vaddr_t vaddr)
{
	vm_map_entry_t key;
	key.start = vaddr;
	return RB_FIND(vm_map_entry_rbtree, &ps->entry_queue, &key);
}

void
vmp_map_dump(vm_map_t *map)
{
	vm_map_entry_t *ent;

	RB_FOREACH (ent, vm_map_entry_rbtree, &map->entry_queue) {
		kdprintf("0x%lx - 0x%lx\n", ent->start, ent->end);
	}
}

int
vm_map_init(vm_map_t *map)
{
	if (map != kernel_process.map)
		vmem_init(&map->vmem, "process map", USER_BASE, USER_SIZE,
		    PGSIZE, NULL, NULL, NULL, 0, 0, kIPL0);

	ke_mutex_init(&map->mutex);
	RB_INIT(&map->entry_queue);
	if (map != kernel_process.map) {
		/* kernel pmap already gets initialised especially */
		pmap_new(map);
	}
	return 0;
}

int
vm_map_allocate(vm_map_t *map, vaddr_t *vaddrp, size_t size, bool exact)
{
	return vm_map_object(map, NULL, vaddrp, size, 0, kVMAll, kVMAll,
	    kVMInheritCopy, exact, false);
}

int
vm_map_object(vm_map_t *map, vm_object_t *object, krx_inout vaddr_t *vaddrp,
    size_t size, voff_t offset, vm_protection_t initial_protection,
    vm_protection_t max_protection, enum vm_inheritance inheritance, bool exact,
    bool copy)
{
	int r;
	kwaitstatus_t w;
	vm_map_entry_t *vad;
	vmem_addr_t addr = exact ? *vaddrp : 0;

	w = ke_wait(&map->mutex, "vm_map_object:map->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	r = vmem_xalloc(&map->vmem, size, 0, 0, 0, addr, 0,
	    exact ? kVMemExact : 0, &addr);
	if (r < 0) {
		kdprintf("vm_map_object failed at vmem_xalloc with %d\n", r);
	}

	if (object != NULL)
		obj_direct_retain(object);

	vad = kmem_alloc(sizeof(vm_map_entry_t));
	vad->start = (vaddr_t)addr;
	vad->end = addr + size;
	vad->offset = offset;
	vad->inheritance = inheritance;
	vad->protection = initial_protection;

	if (object != NULL && copy) {
		vad->has_anonymous = true;

		if (object->is_anonymous) {
			kfatal("implement this by copying amap");
		} else {
			vad->object = object;
			vad->has_anonymous = true;
			vmp_amap_init(map, &vad->amap);
		}

	} else if (object != NULL) {
		vad->object = object;
		vad->has_anonymous = false;
	} else {
		kassert(!copy);
		vad->object = NULL;
		vad->has_anonymous = true;
		vmp_amap_init(map, &vad->amap);
	}

#if 0
	kdprintf("%p: Mapping object at 0x%lx\n", map, vad->start);
#endif

	RB_INSERT(vm_map_entry_rbtree, &map->entry_queue, vad);

	ke_mutex_release(&map->mutex);

	*vaddrp = addr;

	return 0;
}

int
vm_map_deallocate(vm_map_t *map, vaddr_t start, size_t size)
{
	vm_map_entry_t *entry, *tmp;
	vaddr_t end = start + size;
	kwaitstatus_t w;

	w = ke_wait(&map->mutex, "map_section_view:map->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	RB_FOREACH_SAFE (entry, vm_map_entry_rbtree, &map->entry_queue, tmp) {
		if ((entry->start < start && entry->end <= start) ||
		    (entry->start >= end))
			continue;
		else if (entry->start >= start && entry->end <= end) {
			int r;
			ipl_t ipl;

			r = vmem_xfree(&map->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			RB_REMOVE(vm_map_entry_rbtree, &map->entry_queue,
			    entry);

			ipl = vmp_acquire_pfn_lock();
			pmap_unenter_pageable_range(map, entry->start,
			    entry->end);
			vmp_release_pfn_lock(ipl);

			if (entry->object)
				obj_direct_release(entry->object);

			kmem_free(entry, sizeof(vm_map_entry_t));
		} else if (entry->start >= start && entry->end <= end) {
			kfatal("unimplemented deallocate right of vadt\n");
		} else if (entry->start < start && entry->end < end) {
			kfatal("unimplemented other sort of deallocate\n");
		}
	}

	ke_mutex_release(&map->mutex);

	return 0;
}