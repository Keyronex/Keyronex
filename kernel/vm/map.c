#include "kdk/kmem.h"
#include "kdk/vm.h"
#include "kdk/executive.h"
#include "vmp.h"

void vmp_md_ps_init(eprocess_t *ps);
int vmp_vad_cmp(vm_map_entry_t *x, vm_map_entry_t *y);

RB_GENERATE(vm_map_entry_rbtree, vm_map_entry, rb_entry, vmp_vad_cmp);

int
vmp_vad_cmp(vm_map_entry_t *x, vm_map_entry_t *y)
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

vm_map_entry_t *
vmp_ps_vad_find(vm_procstate_t *ps, vaddr_t vaddr)
{
	vm_map_entry_t key;
	key.start = vaddr;
	return RB_FIND(vm_map_entry_rbtree, &ps->vad_queue, &key);
}

int
vm_ps_allocate(vm_procstate_t *vmps, vaddr_t *vaddrp, size_t size, bool exact)
{
	return vm_ps_map_object_view(vmps, NULL, vaddrp, size, 0, kVMAll,
	    kVMAll, false, false, exact);
}

int
vm_ps_map_object_view(vm_procstate_t *vmps, vm_object_t *object, vaddr_t *vaddrp,
    size_t size, uint64_t offset, vm_protection_t initial_protection,
    vm_protection_t max_protection, bool inherit_shared, bool cow, bool exact)
{
	int r;
	vm_map_entry_t *map_entry;
	vmem_addr_t addr = exact ? *vaddrp : 0;

	kassert(size % PGSIZE == 0);
	kassert(offset % PGSIZE == 0);

	ke_wait(&vmps->mutex, "map_object_view:vmps->mutex", false, false, -1);

	r = vmem_xalloc(&vmps->vmem, size, 0, 0, 0, addr, 0,
	    exact ? kVMemExact : 0, &addr);
	if (r < 0) {
		kfatal("vm_ps_map_object_view failed at vmem_xalloc: %d\n", r);
	}

	map_entry = kmem_alloc(sizeof(vm_map_entry_t));
	map_entry->start = (vaddr_t)addr;
	map_entry->end = addr + size;
	map_entry->flags.cow = cow;
	map_entry->flags.offset = offset / PGSIZE;
	map_entry->flags.inherit_shared = inherit_shared;
	map_entry->flags.protection = initial_protection;
	map_entry->flags.max_protection = max_protection;
	map_entry->object = object;

	RB_INSERT(vm_map_entry_rbtree, &vmps->vad_queue, map_entry);

	ke_mutex_release(&vmps->mutex);

	*vaddrp = addr;

	return 0;
}

int
vm_ps_deallocate(vm_procstate_t *vmps, vaddr_t start, size_t size)
{
	vm_map_entry_t *entry, *tmp;
	vaddr_t end = start + size;
	kwaitresult_t w;

	w = ke_wait(&vmps->mutex, "vm_ps_deallocate:vmps->mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	RB_FOREACH_SAFE (entry, vm_map_entry_rbtree, &vmps->vad_queue, tmp) {
		if ((entry->start < start && entry->end <= start) ||
		    (entry->start >= end))
			continue;
		else if (entry->start >= start && entry->end <= end) {
			int r;
			// ipl_t ipl;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			RB_REMOVE(vm_map_entry_rbtree, &vmps->vad_queue, entry);

			kfatal("unimplemented\n");

#if 0
			ipl = vmp_acquire_pfn_lock();
			vmp_wsl_remove_range(vmps, entry->start, entry->end);
			vmp_release_pfn_lock(ipl);

			obj_direct_release(entry->object);
			kmem_free(entry, sizeof(vm_map_entry_t));
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

void
vm_ps_init(eprocess_t *ps)
{
	vm_procstate_t *vmps = ps->vm;

	if (vmps == &kernel_procstate)
		vmem_init(&vmps->vmem, "kernel-dynamic-va", KVM_DYNAMIC_BASE,
		    KVM_DYNAMIC_SIZE, PGSIZE, NULL, NULL, NULL, 0,
		    kVMemBootstrap, kIPL0);
	else
		vmem_init(&vmps->vmem, "dynamic-va", LOWER_HALF,
		    LOWER_HALF_SIZE, PGSIZE, NULL, NULL, NULL, 0,
		    kVMemBootstrap, kIPL0);

	ke_mutex_init(&vmps->mutex);
	RB_INIT(&vmps->vad_queue);
	RB_INIT(&vmps->wsl.tree);
	TAILQ_INIT(&vmps->wsl.queue);
	vmps->wsl.locked_count = 0;
	vmps->wsl.ws_current_count = 0;
	vmps->wsl.max = 6;
	vmps->last_trim_counter = 0;

	vmp_md_ps_init(ps);
}
