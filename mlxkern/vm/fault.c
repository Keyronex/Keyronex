/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 17 2023.
 */

#include "process/ps.h"
#include "vm/vm_internal.h"

vm_fault_return_t
vm_fault(vm_procstate_t *vmps, vaddr_t vaddr, vm_fault_flags_t flags,
    vm_page_t *out)
{
	kwaitstatus_t w;
	vm_vad_t *vad;

	w = ke_wait(&vmps->mutex, "vm_fault:vmps->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	vad = vi_ps_vad_find(vmps, vaddr);
	if (!vad) {
		kfatal("vm_fault: no VAD at address 0x%lx\n", vaddr);
	}

	ke_mutex_release(&vmps->mutex);
	return kMMFaultRetOK;
}