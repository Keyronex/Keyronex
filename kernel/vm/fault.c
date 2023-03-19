/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 17 2023.
 */
/*!
 * @file vm/fault.c
 * @brief Page fault handling.
 *
 * General note: If `out` is specified in any of these functions, the page will
 * be written to there with its reference count incremented.
 *
 * note: we may want a "wsl_update_protection"? instead of wsl_remove and
 * wsl_insert always. This will let us deal with the case of wired pages, which
 * can't be removed.
 */

#include "kdk/devmgr.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vmem.h"
#include "vm/vm_internal.h"

RB_GENERATE(vmp_objpage_rbtree, vmp_objpage, rbtree_entry, vmp_objpage_cmp);

int
vmp_objpage_cmp(struct vmp_objpage *x, struct vmp_objpage *y)
{
	return x->page_index - y->page_index;
}

static vm_fault_return_t
fault_anonymous(vm_map_t *map, vaddr_t vaddr, vm_protection_t protection,
    vm_object_t *section, voff_t offset, vm_fault_flags_t flags,
    vm_page_t **out)
{

}

vm_fault_return_t
vm_fault(vm_map_t *map, vaddr_t vaddr, vm_fault_flags_t flags,
    vm_page_t **out)
{
	vm_fault_return_t r;
	kwaitstatus_t w;
	vm_map_entry_t *vad;
	voff_t offset;

	w = ke_wait(&map->mutex, "vm_fault:map->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	vad = vmp_map_find(map, vaddr);
	if (vad == NULL) {
		kdprintf("vm_fault: no VAD at address 0x%lx\n", vaddr);
		return kVMFaultRetFailure;
	}

	/* verify protection permits */
	if (flags & kVMFaultWrite && !(vad->protection & kVMWrite)) {
		kfatal("vm_fault: write fault at 0x%lx in non-writeable VAD\n",
		    vaddr);
	}

	offset = vaddr - vad->start;


	if (r != kVMFaultRetOK) {
		/* locks have already been dropped */
		return r;
	}

	ke_mutex_release(&map->mutex);

	return kVMFaultRetOK;
}