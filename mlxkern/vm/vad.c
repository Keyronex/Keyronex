/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 17 2023.
 */

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "vm/vm_internal.h"

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

	return vm_ps_map_section_view(vmps, section, vaddrp, size, 0, kVMAll,
	    kVMAll, kVADInheritCopy, exact);
}

int
vm_ps_map_section_view(vm_procstate_t *vmps, vm_section_t *section,
    mlx_in mlx_out vaddr_t *vaddrp, size_t size, voff_t offset,
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