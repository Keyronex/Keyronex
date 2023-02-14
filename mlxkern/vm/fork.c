/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Sun Feb 12 2023.
 */

#include "vm.h"
#include "hl/amd64/amd64.h"

int
vm_ps_fork(vm_procstate_t *vmps, vm_procstate_t *vmps_new)
{
	vm_vad_t *vad;

	/* todo: lock vad list */

	TAILQ_FOREACH (vad, &vmps->vad_queue, vad_queue_entry) {
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
	uint64_t val = (uint64_t)vmps->md.cr3;
	write_cr3(val);
}