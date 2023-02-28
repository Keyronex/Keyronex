/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Feb 12 2023.
 */

#include "kdk/kmem.h"
#include "machdep/amd64/amd64.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "vm_internal.h"

/*!
 * @brief Make a virtual copy of an anonymous section.
 *
 * This is currently used only for POSIX fork(), which is why it's here
 * in fork.c.
 *
 * For copying anonymous sections, which includes private mappings of files
 * (these are internally implemented as anonymous sections with a file parent),
 * it's necessary to specify the process context and associated VAD from which
 * the copy is being made, so that working set list entries which refer to pages
 * that are to now become copy-on-write will be made read-only as appropriate to
 * ensure copy-on-write semantics.
 *
 * In the long term: we need intelligent copying. If anonymous memory was
 * allocated in POSIX land by mmap then we can copy only that part of the
 * anonymous object to which the VAD refers. Meanwhile for a native API, what if
 * a single anonymous object is deliberately referenced by multiple VADs?
 *
 * @pre PFNDB lock held.
 * @pre \p vmps_from vad mutex held.
 */
int
vmp_section_anonymous_copy(vm_procstate_t *vmps_from, vm_procstate_t *vmps_to,
    vm_vad_t *from_vad, vm_section_t *section, vm_section_t **out)
{
	int r;
	struct vmp_page_ref *ref;
	vm_section_t *new_section;

	r = vm_section_new_anonymous(vmps_to, section->size, &new_section);
	kassert(r == 0);

	RB_FOREACH (ref, vmp_page_ref_rbtree, &section->page_ref_rbtree) {
		struct vmp_page_ref *new_ref =
		    kmem_xalloc(sizeof(struct vmp_page_ref), kVMemPFNDBHeld);
		new_ref->page_index = ref->page_index;
		new_ref->vpage = ref->vpage;
		new_ref->vpage->refcount++;
		RB_INSERT(vmp_page_ref_rbtree, &new_section->page_ref_rbtree,
		    new_ref);
	}

	pmap_protect_range(vmps_from, from_vad->start, from_vad->end);

	*out = new_section;

	return 0;
}

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

void
vm_ps_activate(vm_procstate_t *vmps)
{
	kassert((uintptr_t)vmps >= HHDM_BASE);
	uint64_t val = (uint64_t)vmps->md.cr3;
	write_cr3(val);
}