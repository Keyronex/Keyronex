/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 17 2023.
 */

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

/*!
 * @brief Map a view of a section into a process.
 */
int
vm_ps_map_section_view(vm_procstate_t *ps, vm_section_t *section,
    mlx_in mlx_out vaddr_t *vaddrp, size_t size, voff_t offset,
    vm_protection_t initial_protection, vm_protection_t max_protection,
    enum vm_vad_inheritance inheritance)
{
	return 0;
}