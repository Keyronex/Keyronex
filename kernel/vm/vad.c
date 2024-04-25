#include "kdk/kmem.h"
#include "vmp.h"

int vmp_vad_cmp(vm_vad_t *x, vm_vad_t *y);

RB_GENERATE(vm_vad_rbtree, vm_vad, rb_entry, vmp_vad_cmp);

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
vm_ps_allocate(vm_procstate_t *vmps, vaddr_t *vaddrp, size_t size, bool exact)
{
	return vm_ps_map_section_view(vmps, NULL, vaddrp, size, 0, kVMAll,
	    kVMAll, false, false, exact);
}

int
vm_ps_map_section_view(vm_procstate_t *vmps, void *section, vaddr_t *vaddrp,
    size_t size, uint64_t offset, vm_protection_t initial_protection,
    vm_protection_t max_protection, bool inherit_shared, bool cow, bool exact)
{
	int r;
	vm_vad_t *vad;
	vmem_addr_t addr = exact ? *vaddrp : 0;

	ke_wait(&vmps->mutex, "map_section_view:vmps->mutex", false, false, -1);

	r = vmem_xalloc(&vmps->vmem, size, 0, 0, 0, addr, 0,
	    exact ? kVMemExact : 0, &addr);
	if (r < 0) {
		kfatal("vm_ps_map_section_view failed at vmem_xalloc: %d\n", r);
	}

	vad = kmem_alloc(sizeof(vm_vad_t));
	vad->start = (vaddr_t)addr;
	vad->end = addr + size;
	vad->flags.cow = cow;
	vad->flags.offset = offset;
	vad->flags.inherit_shared = inherit_shared;
	vad->flags.protection = initial_protection;
	vad->flags.max_protection = max_protection;
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
	kwaitresult_t w;

	w = ke_wait(&vmps->mutex, "vm_ps_deallocate:vmps->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	RB_FOREACH_SAFE (entry, vm_vad_rbtree, &vmps->vad_queue, tmp) {
		if ((entry->start < start && entry->end <= start) ||
		    (entry->start >= end))
			continue;
		else if (entry->start >= start && entry->end <= end) {
			int r;
			// ipl_t ipl;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			RB_REMOVE(vm_vad_rbtree, &vmps->vad_queue, entry);

			kfatal("unimplemented\n");

#if 0
			ipl = vmp_acquire_pfn_lock();
			vmp_wsl_remove_range(vmps, entry->start, entry->end);
			vmp_release_pfn_lock(ipl);

			obj_direct_release(entry->section);
			kmem_free(entry, sizeof(vm_vad_t));
#endif
		} else if (entry->start >= start && entry->end <= end) {
			kfatal("unimplemented deallocate right of vadt\n");
		} else if (entry->start < start && entry->end < end) {
			kfatal("unimplemented other sort of deallocate\n");
		}
	}

	ke_mutex_release(&vmps->mutex);

	return 0;
}
