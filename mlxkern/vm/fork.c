/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Sun Feb 12 2023.
 */

#include "vm_internal.h"
#include "machdep/amd64/amd64.h"
#include "vm/amd64/vm_md.h"

int
vm_ps_fork(vm_procstate_t *vmps, vm_procstate_t *vmps_new)
{
	vm_vad_t *vad;

	/* todo: lock vad list */

	RB_FOREACH (vad, vm_vad_rbtree, &vmps->vad_queue) {
		switch (vad->inheritance) {
		case kVADInheritShared:
		case kVADInheritCopy:
		case kVADInheritStack:
			break;
		}
	}

	return 0;
}

void vm_ps_activate(vm_procstate_t *vmps)
{
	kassert(vmps >= HHDM_BASE);
	uint64_t val = (uint64_t)vmps->md.cr3;
	write_cr3(val);
}