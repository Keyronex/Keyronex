/*!
 * VM Object to Map Entry mapping
 * ------------------------------
 *
 * This is maintained in vm_object::map_entry_list. It exists so that given a
 * VM object, PTEs mapping pages beyond the end of that object - when it has
 * been decided to truncate it - can be chased up and thrown out.
 *
 * It is protected by a mutex there - vm_object::map_entry_list_lock -  as
 * working set mutexes are acquired while that mutex is held. (I don't think
 * there is a straightforward way to avoid holding the mutex across the
 * unmappings.)
 *
 * Truncation can be done without having to release the working set lock,
 * because the only PTEs that can map shared pages (other than fork-shared) in
 * a process are valid ones. Non-valid pages will be reflecting private pages
 * that were brought in copy-on-write. Linux also chases private pages, but as
 * we have pageable page tables, this is more expensive to do, so we don't.
 *
 * There is this lock ordering:
 * - vm_procstate::map_mutex
 * - -> vm_object::map_entry_list_lock
 * -    -> vm_procstate::ws_mutex
 *
 * but I don't believe that it's ever necessary to have these three all locked
 * together. Only the map_mutex and map_entry_list_lock (for map/unmap/remap
 * operations) and the map_entry_list_lock and ws_mutex (for chasing up PTEs to
 * get rid of)
 */

#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
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

static void
object_map_list_insert(vm_object_t *object, vm_map_entry_t *map_entry)
{
	ke_wait(&object->map_entry_list_lock,
	    "vm_ps_map_objcet_view:obj->map_entry_list_lock", false, false, -1);
	LIST_INSERT_HEAD(&object->map_entry_list, map_entry, object_entry);
	ke_mutex_release(&object->map_entry_list_lock);
}

static void
object_map_list_remove(vm_object_t *object, vm_map_entry_t *map_entry)
{
	ke_wait(&object->map_entry_list_lock,
	    "vm_ps_map_objcet_view:obj->map_entry_list_lock", false, false, -1);
	LIST_REMOVE(map_entry, object_entry);
	ke_mutex_release(&object->map_entry_list_lock);
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

	if (object) {
		object_map_list_insert(object, map_entry);
		if (object->kind == kFile)
			vn_retain(object->file.vnode);
		else
			obj_retain(object->anon.object);
	}

	RB_INSERT(vm_map_entry_rbtree, &vmps->vad_queue, map_entry);

	ex_rwlock_release_write(&vmps->map_lock);

	*vaddrp = addr;

	return 0;
}

int
vm_ps_map_physical_view(vm_procstate_t *vmps, vaddr_t *vaddrp, size_t size,
    paddr_t phys, vm_protection_t initial_protection,
    vm_protection_t max_protection, bool exact)
{
	int r;
	vm_map_entry_t *map_entry;
	vmem_addr_t addr = exact ? *vaddrp : 0;
	pte_t *pte = NULL;
	struct vmp_pte_wire_state pte_wire;
	ipl_t ipl;

	kassert(size % PGSIZE == 0);
	kassert(phys % PGSIZE == 0);

	ex_rwlock_acquire_write(&vmps->map_lock,
	    "map_object_view:vmps->map_lock");

	r = vmem_xalloc(&vmps->vmem, size, 0, 0, 0, addr, 0,
	    exact ? kVMemExact : 0, &addr);
	if (r < 0) {
		kfatal("vm_ps_map_object_view failed at vmem_xalloc: %d\n", r);
	}

	map_entry = kmem_alloc(sizeof(vm_map_entry_t));
	map_entry->start = (vaddr_t)addr;
	map_entry->end = addr + size;
	map_entry->flags.cow = false;
	map_entry->flags.inherit_shared = true;
	map_entry->flags.offset = phys / PGSIZE;
	map_entry->flags.inherit_shared = false;
	map_entry->flags.protection = initial_protection;
	map_entry->flags.max_protection = max_protection;
	map_entry->object = NULL;

	*vaddrp = addr;

	ipl = vmp_acquire_pfn_lock();
	for (int i = 0; i < size - 1; i += PGSIZE, addr += PGSIZE) {
		if (i == 0 || ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
			if (pte)
				vmp_pte_wire_state_release(&pte_wire, false);
			r = vmp_wire_pte(kernel_process, addr, 0, &pte_wire,
			    true);
			kassert(r == 0);
			pte = pte_wire.pte;
		}

		vmp_md_pte_create_hw(pte, (phys >> VMP_PAGE_SHIFT) + i / PGSIZE,
		    true, false, false, false);
		vmp_pagetable_page_noswap_pte_created(vmps,
		    pte_wire.pgtable_pages[0], true);
	}
	vmp_release_pfn_lock(ipl);

	RB_INSERT(vm_map_entry_rbtree, &vmps->vad_queue, map_entry);

	ex_rwlock_release_write(&vmps->map_lock);

	return 0;
}

int
vm_ps_deallocate(vm_procstate_t *vmps, vaddr_t start, size_t size)
{
	vm_map_entry_t *entry, *tmp;
	vaddr_t end = start + size;

	ex_rwlock_acquire_write(&vmps->map_lock, "vm_ps_deallocate:vmps->map_lock");

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
			object_map_list_remove(entry->object, entry);

			vmp_unmap_range(vmps, entry->start, entry->end);

			if (entry->object != NULL) {
				kassert(entry->object->file.vnode != NULL);
				VN_RELEASE(entry->object->file.vnode,
				    "vm_ps_deallocate");
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

			ke_wait(&entry->object->map_entry_list_lock,
			    "vm_deallocate:obj->map_entry_list_lock", false,
			    false, -1);
			entry->end = start;
			ke_mutex_release(&entry->object->map_entry_list_lock);

			vmp_unmap_range(vmps, start, entry->end);

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

			ke_wait(&entry->object->map_entry_list_lock,
			    "vm_deallocate:obj->map_entry_list_lock", false,
			    false, -1);
			entry->start = end;
			ke_mutex_release(&entry->object->map_entry_list_lock);

			vmp_unmap_range(vmps, entry->start, end);

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

	vmps->n_anonymous = 0;

	vmp_md_ps_init(ps);
	vmp_add_to_balance_set(vmps);
	vmps->last_trim_counter = 0;
}
