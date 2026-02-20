/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file fault.c
 * @brief Virtual memory fault handling.
 */

#include <sys/errno.h>
#include <sys/iop.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/proc.h>

#include <vm/map.h>
#include <vm/page.h>

/*
 * State used when waiting for a pagein to complete, as in collided page faults.
 *
 * A pointer to a pagein_wait (in vm_page->pagein_wait) is guaranteed to
 * continue existing, with a refcount > 0, while holding either or both of:
 * - the vm_rs creation_lock
 * - the vm_object creation_lock (if it's an object page)
 * because these are both reacquired by the faulter before it releases the
 * pagein_wait.
 *
 * The refcount, on the other hand, is atomic. So waiters can release it while
 * holding no locks.
 */
struct pagein_wait {
	atomic_uint_fast32_t refcount;
	kevent_t event;
};

#define MAX_CLUSTER 16

size_t obj_max_readahead(struct obj_pte_wire_state *cursor, pgoff_t pgoff,
    size_t max_pages);
void obj_new_ptes_created(struct obj_pte_wire_state *cursor, size_t n);

/* vfs_viewcache.c */
void viewcache_vmm_get_fault_info(vaddr_t addr, vm_object_t **out_object,
    vaddr_t *out_mapping_start, vaddr_t *out_mapping_end,
    size_t *out_object_offset);

struct fault_info {
	vm_map_t *map;
	vm_rs_t *rs;

	struct vm_map_entry *entry;
	vm_object_t *object;	/* Object being faulted on (if any) */
	bool entry_cow;		/* Is this entry CoW? */
	vm_prot_t prot;		/* Protection of mapping */
	vaddr_t mapping_start;	/* Base of map entry in map */
	vaddr_t mapping_end;	/* End of map entry in map */
	size_t mapping_offset;	/* Offset of mapping into object (if any) */
	size_t object_offset;	/* Overall offset into the object (if any) */

	vaddr_t vaddr;
	vm_prot_t type;

	struct pte_cursor cursor; /* Map PTE wiring.*/
};

static bool
is_userland(vaddr_t addr)
{
	return addr < HIGHER_HALF;
}

static vm_prot_t
userland_prot(vaddr_t vaddr)
{
	return is_userland(vaddr) ? VM_USER : 0;
}

struct pagein_wait *
allocate_pagein_wait(void)
{
	struct pagein_wait *wait = kmem_alloc(sizeof(*wait));
	if (wait == NULL)
		return NULL;
	ke_event_init(&wait->event, false);
	/*
	 * the pagein_wait is not accessible til after we release some
	 * spinlocks, so this can be relaxed.
	 */
	atomic_store_explicit(&wait->refcount, 1, memory_order_relaxed);
	return wait;
}

void
pagein_wait_retain(struct pagein_wait *wait)
{
	atomic_fetch_add_explicit(&wait->refcount, 1, memory_order_relaxed);
}

void
pagein_wait_release(struct pagein_wait *wait)
{
	if (atomic_fetch_sub_explicit(&wait->refcount, 1,
	    memory_order_acq_rel) == 1)
		kmem_free(wait, sizeof(*wait));
}

/*
 * The maximum number of pages that can possibly be read ahead when faulting in
 * a file-backed mapping.
 *
 * This simply checks what is possible, it does not deal in memory use policy.
 * The limit is defined by:
 * - the number of following pages in the mapping
 * - the number of following zero pages in the object
 * - the number of following zero pages in the process PTEs
 *
 * Mappings can't be made that are larger than the file size, and file size is
 * (supposed to be) finally reduced by a truncation only AFTER mappings beyond
 * the new end have been adjusted.
 */
static size_t
max_file_readahead(struct fault_info *info, struct obj_pte_wire_state *cursor)
{
	size_t max_pages = MAX_CLUSTER;
	size_t proc_zero_n = 1;
	pgoff_t pgoff = info->object_offset >> PGSHIFT;

	max_pages = MIN2(max_pages,
	    (info->mapping_end - info->vaddr) >> PGSHIFT);
	if (max_pages == 1)
		return 1;

	max_pages = obj_max_readahead(cursor, pgoff, max_pages);
	if (max_pages == 1)
		return 1;

	for (size_t i = 1; i < max_pages; i++) {
		pte_t *proc_pte = info->cursor.pte + i;

		/* stop if we cross a page boundary */
		if (((uintptr_t)(proc_pte) & (PGSIZE - 1)) == 0)
			break;

		if (pmap_pte_characterise(pmap_load_pte(proc_pte)) !=
		    kPTEKindZero)
			break;

		proc_zero_n++;
	}

	return MIN2(max_pages, proc_zero_n);
}

/*
 * Handle a read fault on a VM object mapping.
 *
 * Returns >1 on success (number of pages read, including any read-ahead.)
 */
static int
do_object_fault(struct fault_info *info,
    struct table_lock_state *proc_creation_lock_state)
{
	pte_t pte;
	struct obj_pte_wire_state objcursor;
	vm_prot_t user_prot = is_userland(info->vaddr) ? VM_USER : 0;
	int r;

	ke_spinlock_exit_nospl(&info->map->stealing_lock);
	ke_spinlock_enter_nospl(&info->object->creation_lock);
	ke_spinlock_enter_nospl(&info->object->stealing_lock);

	r = obj_wire_pte(info->object, &objcursor, info->object_offset,
	    true, proc_creation_lock_state);
	if (r != 0)
		kfatal("Problem wiring object PTE\n");

	if (proc_creation_lock_state->did_unlock) {
		/*
		 * If pagein of tables (can happen for anonymous objects) was
		 * done then the creation lock would've been given up until that
		 * finished.
		 * (For page-waits when memory is low, we'd just be giving up
		 * everything, including the wire on the page table, and
		 * retrying later.)
		 */
		ke_spinlock_exit_nospl(&info->object->stealing_lock);
		ke_spinlock_exit_nospl(&info->object->creation_lock);

		ke_spinlock_enter_nospl(&info->map->creation_lock);
		ke_spinlock_enter_nospl(&info->object->creation_lock);
		ke_spinlock_enter_nospl(&info->object->stealing_lock);

		if (pmap_pte_characterise(pmap_load_pte(info->cursor.pte)) ==
		    kPTEKindZero)
			kfatal("PTE changed while object PTE was wired\n");
	}

	pte = pmap_load_pte(objcursor.pte);

	switch (pmap_pte_characterise(pte)) {
	case kPTEKindZero: {
		pgoff_t obj_pgoff = info->object_offset >> PGSHIFT;
		vm_page_t *page[MAX_CLUSTER];
		iop_t *iop;
		sg_list_t sgl;
		sg_seg_t sg_seg[MAX_CLUSTER];
		size_t count;
		struct pagein_wait *pagewait;

		ke_spinlock_exit_nospl(&info->object->stealing_lock);

		pagewait = allocate_pagein_wait();
		if (pagewait == NULL)
			kfatal("TODO: Back out\n");

		count = max_file_readahead(info, &objcursor);

		for (size_t i = 0; i < count; i++) {
			page[i] = vm_page_alloc(VM_PAGE_FILE, 0, VM_DOMID_LOCAL,
			    0);
			if (page[i] == NULL)
				kfatal("TODO: Reduce count, or back out\n");

			/*
			 * TODO: don't memset here, do it in the read
			 * vop/whatever if there is a short read
			 */
			memset((void *)vm_page_hhdm_addr(page[i]), 0, PGSIZE);

			page[i]->pte = objcursor.pte + i;
			page[i]->owner_obj = info->object;
			page[i]->shared.offset = obj_pgoff + i;
			page[i]->shared.share_count = 1;
			page[i]->pagein_wait = pagewait;

			pmap_pte_soft_create(objcursor.pte + i, kPTEKindBusy,
			    VM_PAGE_PFN(page[i]), false);

			pmap_pte_soft_create(info->cursor.pte + i, kPTEKindBusy,
			    VM_PAGE_PFN(page[i]), false);

			sg_seg[i].paddr = VM_PAGE_PADDR(page[i]);
			sg_seg[i].length = PGSIZE;
		}

		ke_spinlock_exit_nospl(&info->object->creation_lock);
		ke_spinlock_exit_nospl(&info->map->creation_lock);

		splx(IPL_0);

		sgl.elems = sg_seg;
		sgl.elems_n = count;

		iop = iop_new_read(info->object->vnode, &sgl, 0,
		    count << PGSHIFT, obj_pgoff << PGSHIFT);
		iop_send_sync(iop);

#if 0
		kprintf("Did read in object page (%d; tot. %d)\n", obj_pgoff, count);
#endif

		spldisp();
		ke_spinlock_enter_nospl(&info->map->creation_lock);
		ke_spinlock_enter_nospl(&info->map->stealing_lock);
		ke_spinlock_enter_nospl(&info->object->creation_lock);
		ke_spinlock_enter_nospl(&info->object->stealing_lock);

		for (size_t i = 0; i < count; i++) {
			pmap_pte_hwleaf_create(objcursor.pte + i,
			    VM_PAGE_PFN(page[i]), PMAP_L0, 0,
			    kCacheModeDefault);
			pmap_pte_hwleaf_create(info->cursor.pte + i,
			    VM_PAGE_PFN(page[i]), PMAP_L0, info->prot,
			    kCacheModeDefault);
		}

		obj_new_ptes_created(&objcursor, count);
		obj_unwire_pte(info->object, &objcursor);
		ke_spinlock_exit_nospl(&info->object->stealing_lock);
		ke_spinlock_exit_nospl(&info->object->creation_lock);

		info->rs->valid_n += count;
		pmap_new_leaf_valid_ptes_created(info->rs, &info->cursor, count);
		pmap_unwire_pte(info->map, info->rs, &info->cursor);
		ke_spinlock_exit_nospl(&info->map->stealing_lock);
		ke_spinlock_exit_nospl(&info->map->creation_lock);

		ke_event_set_signalled(&pagewait->event, true);
		pagein_wait_release(pagewait);

		return count;

	}

	case kPTEKindHW: {
		vm_page_t *page = pmap_pte_hwleaf_page(pte, 0);
		vm_prot_t prot = VM_READ;

		if (++page->shared.share_count == 1)
			vm_page_retain(page);

		obj_unwire_pte(info->object, &objcursor);
		ke_spinlock_exit_nospl(&info->object->stealing_lock);
		ke_spinlock_exit_nospl(&info->object->creation_lock);

		ke_spinlock_enter_nospl(&info->map->stealing_lock);

		if (info->type & VM_WRITE &&
		    info->prot & VM_WRITE && !info->entry_cow)
		    prot |= VM_WRITE;
		pmap_pte_hwleaf_create(info->cursor.pte, VM_PAGE_PFN(page), 0,
		    prot | user_prot, kCacheModeDefault);
		info->rs->valid_n += 1;
		pmap_new_leaf_valid_ptes_created(info->rs, &info->cursor, 1);
		pmap_unwire_pte(info->map, info->rs, &info->cursor);
		ke_spinlock_exit_nospl(&info->map->stealing_lock);
		ke_spinlock_exit_nospl(&info->map->creation_lock);

		return 1;
	}

	case kPTEKindBusy: {
		struct pagein_wait *pagewait;
		vm_page_t *page = pmap_pte_soft_page(objcursor.pte);

		pagewait = page->pagein_wait;
		pagein_wait_retain(pagewait);

		obj_unwire_pte(info->object, &objcursor);
		ke_spinlock_exit_nospl(&info->object->stealing_lock);
		ke_spinlock_exit_nospl(&info->object->creation_lock);

		ke_spinlock_enter_nospl(&info->map->stealing_lock);
		pmap_unwire_pte(info->map, info->rs, &info->cursor);
		ke_spinlock_exit_nospl(&info->map->stealing_lock);
		ke_spinlock_exit_nospl(&info->map->creation_lock);
		splx(IPL_0);
		ke_rwlock_exit_read(&info->map->map_lock);

		ke_wait1(&pagewait->event, "vm_fault_pagein", false,
		    ABSTIME_FOREVER);
		pagein_wait_release(pagewait);

		return -EAGAIN;
	}

	default:
		kfatal("Implement me\n");
	}

	kfatal("Do object fault\n");
}

static int
do_fork_fault(struct fault_info *info)
{
	struct vm_anon *anon = pmap_pte_soft_anon(pmap_load_pte(info->cursor.pte));
	pte_t anonpte;

	ke_spinlock_enter_nospl(&anon_creation_lock);
	ke_spinlock_enter_nospl(&anon_stealing_lock);

	anonpte = pmap_load_pte(&anon->pte);

	switch (pmap_pte_characterise(anonpte)) {
	case kPTEKindHW: {
		vm_page_t *page = pmap_pte_hwleaf_page(anonpte, PMAP_L0);
		vm_prot_t prot;
		if (++page->shared.share_count == 1)
			vm_page_retain(page);

		ke_spinlock_exit_nospl(&anon_stealing_lock);
		ke_spinlock_exit_nospl(&anon_creation_lock);

		prot = VM_READ | userland_prot(info->vaddr);
		if (info->prot & VM_EXEC)
			prot |= VM_EXEC;

		pmap_pte_hwleaf_create(info->cursor.pte, VM_PAGE_PFN(page),
		    PMAP_L0, prot, kCacheModeDefault);
		info->rs->valid_n += 1;
		pmap_anon_ptes_converted_to_leaf_valid_pte(info->rs,
		    &info->cursor, 1);
		pmap_unwire_pte(info->map, info->rs, &info->cursor);
		ke_spinlock_exit_nospl(&info->map->stealing_lock);
		ke_spinlock_exit_nospl(&info->map->creation_lock);

		return 1;
	}

	default:
		kfatal("Implement me: non-valid pages in vm_anon pte\n");
	}
}

static int
do_dirty_fork_fault(struct fault_info *info, vm_page_t *old_page)
{
	struct vm_anon *anon = old_page->owner_anon;

	ke_spinlock_enter_nospl(&anon_creation_lock);
	ke_spinlock_enter_nospl(&anon_stealing_lock);
	if (anon->refcount == 1) {
		vm_domain_t *dom = &vm_domains[old_page->domain];

		ke_spinlock_enter_nospl(&dom->queues_lock);

		dom->use_n[VM_PAGE_ANON_FORKED]--;
		dom->use_n[VM_PAGE_PRIVATE]++;

		old_page->use = VM_PAGE_PRIVATE;
		old_page->owner_rs = info->rs;
		old_page->pte = info->cursor.pte;
		ke_spinlock_exit_nospl(&dom->queues_lock);
		ke_spinlock_exit_nospl(&anon_stealing_lock);
		ke_spinlock_exit_nospl(&anon_creation_lock);

		kmem_free(anon, sizeof(struct vm_anon));

		pmap_pte_hwleaf_set_writeable(info->cursor.pte);

		info->rs->private_pages_n++;
	} else {
		vm_page_t *new_page;
		vm_prot_t prot;

		vm_page_retain(old_page);
		ke_spinlock_exit_nospl(&anon_stealing_lock);
		ke_spinlock_exit_nospl(&info->map->stealing_lock);

		new_page = vm_page_alloc(VM_PAGE_PRIVATE, 0, VM_DOMID_LOCAL, 0);
		if (new_page == NULL)
			kfatal("TODO: Wait on pages avail\n");

		new_page->pte = info->cursor.pte;
		new_page->owner_rs = info->rs;

		memcpy((void *)vm_page_hhdm_addr(new_page),
		    (void *)vm_page_hhdm_addr(old_page), PGSIZE);

		anon->refcount--;
		if (--old_page->shared.share_count == 0)
			vm_page_release(old_page);
		ke_spinlock_exit_nospl(&anon_creation_lock);

		ke_spinlock_enter_nospl(&info->map->stealing_lock);

		/* paranoid? */
		kassert(pmap_pte_characterise(pmap_load_pte(info->cursor.pte))
		    == kPTEKindHW);
		kassert(pmap_pte_hwleaf_page(pmap_load_pte(info->cursor.pte),
		    0) == old_page);


		prot = VM_READ | VM_WRITE | userland_prot(info->vaddr);
		if (info->prot & VM_EXEC)
			prot |= VM_EXEC;

		pmap_pte_hwleaf_create(info->cursor.pte, VM_PAGE_PFN(new_page),
		    PMAP_L0, prot, kCacheModeDefault);
		info->rs->private_pages_n++;
		pmap_tlb_flush_vaddr_globally(info->vaddr);

		vm_page_release(old_page);
	}

	pmap_unwire_pte(info->map, info->rs, &info->cursor);
	ke_spinlock_exit_nospl(&info->map->stealing_lock);
	ke_spinlock_exit_nospl(&info->map->creation_lock);

	return 0;
}


int
vm_fault(vaddr_t addr, vm_prot_t type)
{
	struct fault_info info;
	ipl_t ipl;
	int ret;
	pte_t pte;

#if 0
	kprintf("[pid %d] vm_fault on %p\n", curproc()->pid, addr);
#endif

	addr = rounddown2(addr, PGSIZE);

	/* Initialize fault info. */
	info.vaddr = addr;
	info.type = type;

	if (addr >= HIGHER_HALF) {
		info.map = proc0.vm_map;
		info.rs = &proc0.vm_map->rs;
	} else {
		info.map = thread_vm_map(curthread());
		info.rs = &info.map->rs;
	}

	ke_rwlock_enter_read(&info.map->map_lock, "vm_fault");

	if (addr >= FILE_MAP_BASE && addr < FILE_MAP_BASE + FILE_MAP_SIZE) {
		info.entry = NULL;
		info.prot = VM_READ | VM_WRITE;
		info.entry_cow = false;

		viewcache_vmm_get_fault_info(addr, &info.object,
		    &info.mapping_start, &info.mapping_end,
		    &info.mapping_offset);
	} else {
		struct vm_map_entry *entry = vm_map_lookup(info.map, addr);
		if (entry == NULL)
			kfatal("vm_fault: no entry found for 0x%zx\n", addr);

		info.entry = entry;
		info.object = entry->object;
		info.prot = entry->prot;
		info.entry_cow = entry->cow;
		info.mapping_start = entry->start;
		info.mapping_end = entry->end;
		info.mapping_offset = entry->offset;
	}

	info.object_offset = info.mapping_offset + (addr - info.mapping_start);

	if (type & VM_WRITE && (info.prot & VM_WRITE) == 0)
		kfatal("vm_fault: write access to read-only mapping\n");

	ipl = spldisp();
	ke_spinlock_enter_nospl(&info.map->creation_lock);
	ke_spinlock_enter_nospl(&info.map->stealing_lock);
	pmap_wire_pte(info.map, info.rs, &info.cursor, addr, true);

	pte = pmap_load_pte(info.cursor.pte);

// retry:
	switch (pmap_pte_characterise(pte)) {
	case kPTEKindHW: {
#ifdef PMAP_SW_A_BIT
		if (!pmap_pte_hw_is_accessed(info.cursor.pte)) {
			pmap_pte_hw_set_accessed(info.cursor.pte);
			ke_spinlock_exit_nospl(&info.map->stealing_lock);
			ke_spinlock_exit_nospl(&info.map->creation_lock);
			ret = 0;
			break;
		}
#endif
		if (type & VM_EXEC &&
		    !pmap_pte_hwleaf_executable(pte)) {
			kassert(!(info.prot & VM_EXEC));
			kfatal("Attempt to execute non-executable page\n");
			break;
		} else if ((type & VM_WRITE) &&
		    !pmap_pte_hwleaf_writeable(pte)) {
			/*
			 * Write fault: map entry permits write, PTE is valid,
			 * but not writeable.
			 * Possibilities:
			 * - page is legally writeable, but PTE is not set
			 *   writeable because of dirty-bit emulation;
			 * - map entry is CoW;
			 * - this is a forked page.
			 */
			vm_page_t *old_page = pmap_pte_hwleaf_page(pte,
			    PMAP_L0);

			if (old_page->use == VM_PAGE_ANON_FORKED) {
				ret = do_dirty_fork_fault(&info, old_page);
				break;
			} else if (info.entry_cow) {
				vm_page_t *new_page;

				/* must retain before we drop stealing lock! */
				vm_page_retain(old_page);

				ke_spinlock_exit_nospl(&info.map->stealing_lock);

				new_page = vm_page_alloc(VM_PAGE_PRIVATE, 0,
				    VM_DOMID_LOCAL, 0);
				if (new_page == NULL)
					kfatal("TODO: Wait on pages avail\n");

				ke_spinlock_enter_nospl(&info.map->stealing_lock);

				new_page->pte = info.cursor.pte;
				new_page->owner_rs = info.rs;

				memcpy((void *)vm_page_hhdm_addr(new_page),
				    (void *)vm_page_hhdm_addr(old_page),
				    PGSIZE);

				/*
				 * FIXME: what if it was already evicted while
				 * lock dropped?
				 */
				rs_evict_leaf_pte(info.rs, info.vaddr, old_page,
				    info.cursor.pte);

				pmap_pte_hwleaf_create(info.cursor.pte,
				    VM_PAGE_PFN(new_page), PMAP_L0,
				    VM_READ | (info.prot & VM_EXEC) |
					userland_prot(info.vaddr),
				    kCacheModeDefault);

				info.rs->private_pages_n++;
				pmap_tlb_flush_vaddr_globally(info.vaddr);

				pmap_unwire_pte(info.map, info.rs,
				    &info.cursor);
				ke_spinlock_exit_nospl(&info.map->stealing_lock);
				ke_spinlock_exit_nospl(&info.map->creation_lock);

				vm_page_release(old_page);

				ret = 0;
				break;
			} else {
				/* dirty-bit emulation */
				pmap_pte_hwleaf_set_writeable(info.cursor.pte);
				pmap_unwire_pte(info.map, info.rs,
				    &info.cursor);
				ke_spinlock_exit_nospl(&info.map->stealing_lock);
				ke_spinlock_exit_nospl(&info.map->creation_lock);
				ret = 0;
				break;
			}
			break;
		} else {
			kfatal("Nothing to be done? type 0x%x\n", type);
		}
	}

	case kPTEKindZero: {
		vm_page_t *page;

		if (info.object != NULL) {
			struct table_lock_state lock_state = {
				.lock = &info.map->creation_lock,
				.did_unlock = false,
			};
			ret = do_object_fault(&info, &lock_state);
			if (ret == -EAGAIN)
				return -EAGAIN;
			break;
		}

		/* otherwise, no object - anonymous fault */
		info.rs->private_pages_n++;

		ke_spinlock_exit_nospl(&info.map->stealing_lock);

		page = vm_page_alloc(VM_PAGE_PRIVATE, 0, VM_DOMID_LOCAL, 0);
		if (page == NULL)
			kfatal("TODO: Wait on pages avail event\n");

		memset((void *)vm_page_hhdm_addr(page), 0, PGSIZE);

		ke_spinlock_enter_nospl(&info.map->stealing_lock);

		page->pte = info.cursor.pte;
		page->owner_rs = info.rs;

		pmap_pte_hwleaf_create(info.cursor.pte, VM_PAGE_PFN(page),
		    PMAP_L0,
		    VM_READ | (type & VM_WRITE) | (info.prot & VM_EXEC) |
			userland_prot(info.vaddr),
		    kCacheModeDefault);

		pmap_new_leaf_valid_ptes_created(info.rs, &info.cursor, 1);
		info.rs->valid_n += 1;

		pmap_unwire_pte(info.map, info.rs, &info.cursor);
		ke_spinlock_exit_nospl(&info.map->stealing_lock);
		ke_spinlock_exit_nospl(&info.map->creation_lock);
		ret = 0;

		break;
	}

	case kPTEKindBusy: {
		vm_page_t *page = pmap_pte_soft_page(info.cursor.pte);

		pagein_wait_retain(page->pagein_wait);
		pmap_unwire_pte(info.map, info.rs, &info.cursor);
		ke_spinlock_exit_nospl(&info.map->stealing_lock);
		ke_spinlock_exit_nospl(&info.map->creation_lock);
		splx(ipl);
		ke_rwlock_exit_read(&info.map->map_lock);

		ke_wait1(&page->pagein_wait->event, "vm_fault_pagein", false,
		    ABSTIME_FOREVER);

		pagein_wait_release(page->pagein_wait);

		return -EAGAIN;
	}

	case kPTEKindTrans: {
		kfatal("Handle transition PTE\n");
	}

	case kPTEKindFork: {
		ret = do_fork_fault(&info);
		break;
	}

	default:
		kfatal("Implement me!\n");
	}

	splx(ipl);
	ke_rwlock_exit_read(&info.map->map_lock);

	return ret;
}
