/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sun Feb 12 2023.
 */

#include "kdk/kmem.h"
#include "machdep/amd64/amd64.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "vm_internal.h"

#if 0

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
vmp_section_anonymous_copy(vm_map_t *map_from, vm_map_t *map_to,
    vm_map_entry_t *from_vad, vm_object_t *section, vm_object_t **out)
{
	int r;
	struct vmp_objpage *ref;
	vm_object_t *new_section;

	r = vm_object_new_anonymous(map_to, section->size, &new_section);
	kassert(r == 0);

	RB_FOREACH (ref, vmp_objpage_rbtree, &section->page_ref_rbtree) {
		struct vmp_objpage *new_ref =
		    kmem_xalloc(sizeof(struct vmp_objpage), kVMemPFNDBHeld);
		new_ref->page_index = ref->page_index;
		new_ref->vpage = ref->vpage;
		new_ref->vpage->refcount++;
		RB_INSERT(vmp_objpage_rbtree, &new_section->page_ref_rbtree,
		    new_ref);
	}

	pmap_protect_range(map_from, from_vad->start, from_vad->end);

	*out = new_section;

	return 0;
}

int
vm_map_fork(vm_map_t *map, vm_map_t *map_new)
{
	vm_map_entry_t *vad;

	/* todo: lock vad list */

	RB_FOREACH (vad, vm_map_entry_rbtree, &map->entry_queue) {
		switch (vad->inheritance) {
		case kVMInheritShared:
		case kVMInheritCopy:
		case kVMInheritStack:
			break;
		}
	}

	return 0;
}
#endif

void
vm_map_activate(vm_map_t *map)
{
	kassert((uintptr_t)map >= HHDM_BASE);
	uint64_t val = (uint64_t)map->md.cr3;
	write_cr3(val);
}