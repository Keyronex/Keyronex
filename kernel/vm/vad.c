/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 17 2023.
 */

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/vm.h"
#include "vm/vm_internal.h"

#if 0

RB_GENERATE(vm_vad_rbtree, vm_vad, rbtree_entry, vmp_vad_cmp);

int
vmp_vad_cmp(vm_vad_t *x, vm_vad_t *y)
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

vm_vad_t *
vmp_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr)
{
	vm_vad_t key;
	key.start = vaddr;
	return RB_FIND(vm_vad_rbtree, &ps->vad_queue, &key);
}

int
vm_section_new_anonymous(vm_procstate_t *vmps, size_t size, vm_section_t **out)
{
	vm_section_t *section = kmem_alloc(sizeof(vm_section_t));

	obj_initialise_header(&section->header, kObjTypeSection);

	section->kind = kSectionAnonymous;
	section->size = size;
	section->parent = NULL;
	RB_INIT(&section->page_ref_rbtree);

	*out = section;

	return 0;
}

int
vm_ps_allocate(vm_procstate_t *vmps, vaddr_t *vaddrp, size_t size, bool exact)
{
	vm_section_t *section;
	int r;

	r = vm_section_new_anonymous(vmps, size, &section);
	kassert(r == 0);

	r = vm_ps_map_section_view(vmps, section, vaddrp, size, 0, kVMAll,
	    kVMAll, kVADInheritCopy, exact);

	obj_direct_release(section);

	return r;
}

int
vm_ps_map_section_view(vm_procstate_t *vmps, vm_section_t *section,
    krx_in krx_out vaddr_t *vaddrp, size_t size, voff_t offset,
    vm_protection_t initial_protection, vm_protection_t max_protection,
    enum vm_vad_inheritance inheritance, bool exact)
{
	int r;
	kwaitstatus_t w;
	vm_vad_t *vad;
	vmem_addr_t addr = exact ? *vaddrp : 0;

	w = ke_wait(&vmps->mutex, "map_section_view:vmps->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	r = vmem_xalloc(&vmps->vmem, size, 0, 0, 0, addr, 0,
	    exact ? kVMemExact : 0, &addr);
	if (r < 0) {
		kdprintf(
		    "vm_ps_map_section_view failed at vmem_xalloc with %d\n",
		    r);
	}

	obj_direct_retain(&section->header);

	vad = kmem_alloc(sizeof(vm_vad_t));
	vad->start = (vaddr_t)addr;
	vad->end = addr + size;
	vad->offset = offset;
	vad->inheritance = inheritance;
	vad->protection = initial_protection;
	vad->section = section;

	RB_INSERT(vm_vad_rbtree, &vmps->vad_queue, vad);

	ke_mutex_release(&vmps->mutex);

	*vaddrp = addr;

	return 0;
}

int
vm_ps_deallocate(vm_procstate_t *vmps, vaddr_t start, size_t size)
{
	vm_vad_t *entry, *tmp;
	vaddr_t end = start + size;
	kwaitstatus_t w;

	w = ke_wait(&vmps->mutex, "map_section_view:vmps->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	RB_FOREACH_SAFE (entry, vm_vad_rbtree, &vmps->vad_queue, tmp) {
		if ((entry->start < start && entry->end <= start) ||
		    (entry->start >= end))
			continue;
		else if (entry->start >= start && entry->end <= end) {
			int r;
			ipl_t ipl;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			RB_REMOVE(vm_vad_rbtree, &vmps->vad_queue, entry);

			ipl = vmp_acquire_pfn_lock();
			vmp_wsl_remove_range(vmps, entry->start, entry->end);
			vmp_release_pfn_lock(ipl);

			obj_direct_release(entry->section);
			kmem_free(entry, sizeof(vm_vad_t));
		} else if (entry->start >= start && entry->end <= end) {
			kfatal("unimplemented deallocate right of vadt\n");
		} else if (entry->start < start && entry->end < end) {
			kfatal("unimplemented other sort of deallocate\n");
		}
	}

	ke_mutex_release(&vmps->mutex);

	return 0;
}
#endif