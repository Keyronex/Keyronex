#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "vmp.h"

void vmp_md_ps_init(eprocess_t *ps);
int vmp_vad_cmp(vm_map_entry_t *x, vm_map_entry_t *y);
void vmp_add_to_balance_set(vm_procstate_t *vmps);

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

	ex_rwlock_acquire_write(&vmps->map_lock, "map_object_view:vmps->map_lock");

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

	ex_rwlock_release_write(&vmps->map_lock);

	*vaddrp = addr;

	return 0;
}

int
vm_ps_deallocate(vm_procstate_t *vmps, vaddr_t start, size_t size)
{
	vm_map_entry_t *entry, *tmp;
	vaddr_t end = start + size;
	kwaitresult_t w;

	ex_rwlock_acquire_write(&vmps->map_lock, "vm_ps_deallocate:vmps->map_lock");
	kassert(w == kKernWaitStatusOK);

	RB_FOREACH_SAFE (entry, vm_map_entry_rbtree, &vmps->vad_queue, tmp) {
		if ((entry->start < start && entry->end <= start) ||
		    (entry->start >= end))
			continue;
		else if (entry->start >= start && entry->end <= end) {
			/* entry wholly encompassed */

			int r;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			RB_REMOVE(vm_map_entry_rbtree, &vmps->vad_queue, entry);

			vmp_unmap_range(vmps, entry->start, entry->end);

			if (entry->object != NULL) {
				kassert(entry->object->file.vnode != NULL);
				kprintf(" -VN- reLEASE in vm_ps_deallocate\n");
				vn_release(entry->object->file.vnode);
			}

			kmem_free(entry, sizeof(vm_map_entry_t));
		} else if (entry->start < start && entry->end > start &&
		    entry->end <= end) {
			/* part of the right side of the entry is encompassed */

			int r;
			vmem_addr_t new_start = entry->start;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			vmp_unmap_range(vmps, start, entry->end);

			entry->end = start;

			r = vmem_xalloc(&vmps->vmem, entry->end - new_start, 0,
			    0, 0, 0, ~0, 0, &new_start);
			kassert(r == 0);
		} else if (entry->start >= start && entry->start < end &&
		    entry->end > end) {
			/* part of the left side of the entry is encompassed */

			int r;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			vmp_unmap_range(vmps, entry->start, end);

			entry->start = end;

			r = vmem_xalloc(&vmps->vmem, entry->end - entry->start, 0,
			    0, 0, 0, ~0, 0, &entry->start);
			kassert(r == 0);
		} else if (entry->start < start && entry->end > end) {
			/* middle of the entry is encompassed */

			int r;
			vm_map_entry_t *new_entry;

			r = vmem_xfree(&vmps->vmem, entry->start,
			    entry->end - entry->start, 0);
			kassert(r == entry->end - entry->start);

			vmp_unmap_range(vmps, start, end);

			new_entry = kmem_alloc(sizeof(vm_map_entry_t));
			kassert(new_entry != NULL);

#if 0
			*new_entry = *entry;
			new_entry->start = end;
			new_entry->flags.offset = entry->flags.offset +
			    (end - entry->start) / PGSIZE;

			RB_INSERT(vm_map_entry_rbtree, &vmps->vad_queue,
			    new_entry);

			entry->end = start;

			r = vmem_xalloc(&vmps->vmem,
			    new_entry->end - new_entry->start, 0, 0, 0, 0, ~0,
			    0, &new_entry->start);
			kassert(r == 0);

			r = vmem_xalloc(&vmps->vmem, entry->end - entry->start,
			    0, 0, 0, 0, ~0, 0, &entry->start);
			kassert(r == 0);
#else
			kfatal("Implement me\n");
#endif
		}
	}

	ex_rwlock_release_write(&vmps->map_lock);

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

	ke_mutex_init(&vmps->map_lock.mutex);
	ke_mutex_init(&vmps->ws_mutex);

	RB_INIT(&vmps->vad_queue);
	vmp_wsl_init(vmps, &vmps->wsl);

	vmp_md_ps_init(ps);
	vmp_add_to_balance_set(vmps);
}
