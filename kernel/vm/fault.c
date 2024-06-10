#include "kdk/dev.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "ubc.h"
#include "vmp.h"
#include "vmp_dynamics.h"

/*!
 * @file fault.c
 * @brief Page fault handling
 *
 * Notes
 * - When a busy PTE is made, an additional reference is kept to the pagetable
 * pte so that it can be consulted afterwards - we need to decide on whether we
 * will be needing this. We will do if we permit expulsion of busy pages, which
 * is not currently possible because the page isn't added to a working set until
 * after the fault was completed, and because we don't have unmapping, but when
 * we do, we might elect to wait for busy pages to not be busy before proceeding
 * with it. That may require thought to square with the creation of some kind
 * of mapping list lock separate from the working set lock.
 */

struct fault_info {
	vm_object_t *object;
	vaddr_t start;
	pgoff_t offset;
	bool copy, writeable;
	bool map_lock_held;
	bool ws_lock_held;
};

void
vmp_pager_state_retain(struct vmp_pager_state *state)
{
	kassert(state->refcnt >= 0 && state->refcnt <= 5);
	__atomic_fetch_add(&state->refcnt, 1, __ATOMIC_RELAXED);
}

void
vmp_pager_state_release(struct vmp_pager_state *state)
{
	kassert(state->refcnt >= 0 && state->refcnt <= 5);
	if (__atomic_fetch_sub(&state->refcnt, 1, __ATOMIC_RELEASE) == 1)
		kmem_free(state, sizeof(struct vmp_pager_state));
}

static int
vmp_do_file_fault(eprocess_t *process, vm_procstate_t *vmps,
    struct fault_info *area_info, struct vmp_pte_wire_state *state,
    vaddr_t vaddr)
{
	vm_object_t *object = area_info->object;
	size_t object_byteoffset = area_info->offset * PGSIZE +
	    (vaddr - area_info->start);
	pgoff_t object_pgoffset = object_byteoffset / PGSIZE;
	struct vmp_pte_wire_state object_state;
	int r;

	r = vmp_wire_pte(process, object_byteoffset, object->vpml4,
	    &object_state, true);
	kassert(r == 0);

	switch (vmp_pte_characterise(object_state.pte)) {
	case kPTEKindValid: {
		vm_page_t *page = vmp_pte_hw_page(object_state.pte, 1);
		pte_t *pte = state->pte;
		vm_page_t *pml1_page = state->pgtable_pages[0];

		vmp_page_retain_locked(page);
		vmp_md_pte_create_hw(pte, page->pfn, false, true);
		vmp_pagetable_page_noswap_pte_created(process->vm, pml1_page, true);
		vmp_pte_wire_state_release(&object_state, true);
		vmp_pte_wire_state_release(state, false);
		vmp_release_pfn_lock(kIPLAST);

		r = vmp_wsl_insert(vmps, vaddr, true);
		if (r == NIL_WSE) {
			kfatal("Working set insertion failed - evict!!\n");
		}

		return kVMFaultRetOK;
	}

	case kPTEKindBusy: {
		vm_page_t *page = vm_pfn_to_page(
		    vmp_md_soft_pte_pfn(state->pte));
		struct vmp_pager_state *pgstate = page->pager_request;

		vmp_pager_state_retain(pgstate);
		vmp_pte_wire_state_release(&object_state, true);
		vmp_pte_wire_state_release(state, false);
		vmp_release_pfn_lock(kIPLAST);

		/* wait for the page-in to complete... */
		if (area_info->map_lock_held) {
			area_info->map_lock_held = false;
			ex_rwlock_release_read(&vmps->map_lock);
		}
		area_info->ws_lock_held = false;
		ke_mutex_release(&vmps->ws_mutex);
		KE_WAIT(&pgstate->event, false, false, -1);

		vmp_pager_state_release(pgstate);

		/* and let the collided fault be retried. */
		return kVMFaultRetRetry;
	}

	case kPTEKindSwap:
	case kPTEKindTrans:
		/* these may become possible with anonymous objects */
		kfatal("impossible\n");

	case kPTEKindZero:
		/* fall out */
	}

	/* not in the cache - try to read it in */
	vm_mdl_t *mdl;
	iop_t *iop;
	struct vmp_pager_state *pgstate;
	vm_page_t *page;

	r = vmp_page_alloc_locked(&page, kPageUseFileShared, false);
	if (r != 0) {
		vmp_pte_wire_state_release(&object_state, true);
		vmp_pte_wire_state_release(state, false);
		vmp_release_pfn_lock(kIPLAST);
		return -kVMFaultRetPageShortage;
	}

	pgstate = kmem_xalloc(sizeof(*pgstate), kVMemPFNDBHeld);
	kassert(r == 0);

	pgstate->refcnt = 1;
	pgstate->vpfn = (vaddr / PGSIZE);
	pgstate->length = 1;
	ke_event_init(&pgstate->event, false);

	page->pager_request = pgstate;
	page->owner = object;
	page->offset = object_pgoffset;
	page->dirty = false;
	page->referent_pte = V2P((vaddr_t)object_state.pte);

	/* create busy PTEs in both the prototype pagetable and the process */
	vmp_md_pte_create_busy(object_state.pte, page->pfn);
	vmp_pagetable_page_noswap_pte_created(process->vm,
	    object_state.pgtable_pages[0], true);

	vmp_md_pte_create_busy(state->pte, page->pfn);
	vmp_pagetable_page_noswap_pte_created(process->vm, state->pgtable_pages[0],
	    true);

	/* additional retains to keep PTE and proto PTE pointers valid */
	vmp_page_retain_locked(object_state.pgtable_pages[0]);
	vmp_page_retain_locked(state->pgtable_pages[0]);

	vmp_pte_wire_state_release(&object_state, true);
	vmp_pte_wire_state_release(state, false);
	vmp_release_pfn_lock(kIPLAST);
	if (area_info->map_lock_held) {
		area_info->map_lock_held = false;
		ex_rwlock_release_read(&vmps->map_lock);
	}
	ke_mutex_release(&vmps->ws_mutex);

	mdl = &pgstate->mdl;
	mdl->nentries = 1;
	mdl->offset = 0;
	mdl->write = true;
	mdl->pages[0] = page;
	iop = iop_new_vnode_read(object->file.vnode, mdl, PGSIZE,
	    object_byteoffset);

	iop_send_sync(iop);
	iop_free(iop);

	/*
	 * we don't need the map_lock, any unmapper will be waiting on our busy
	 * page
	 */

	KE_WAIT(&vmps->ws_mutex, false, false, -1);
	vmp_acquire_pfn_lock();

	switch (vmp_pte_characterise(object_state.pte)) {
	case kPTEKindZero:
		kfatal("Not working yet\n"); /* pager state */
		/* not used yet, but could be useful for e.g. file truncation */
		kassert(page->use == kPageUseDeleted && page->refcnt == 1);
		vmp_page_release_locked(page);
		vmp_page_release_locked(object_state.pgtable_pages[0]);
		vmp_page_release_locked(state->pgtable_pages[0]);

		vmp_release_pfn_lock(kIPLAST);

		return kVMFaultRetRetry;

	case kPTEKindValid:
	case kPTEKindSwap:
	case kPTEKindTrans:
		/*
		 * we might end up permitting this in the future. (for anons)
		 * should be treated like the case of finding a zero PTE?
		 * i.e. give up on it all.
		 * for now it's impossible, but let's be certain
		 */
		kfatal("Impossible\n");

	case kPTEKindBusy:
		/* fall out */
	}
	kassert(vm_pfn_to_page(vmp_md_soft_pte_pfn(state->pte)) == page);
	vmp_md_pte_create_hw(object_state.pte, page->pfn, true, true);

	/*
	 * the process PTE must still be busy - we wait for busy PTEs to become
	 * unbusy when unmapping etc. there is nothing else that can change its
	 * state, as it is not on the working set list yet.
	 *
	 * it could be worthwhile in the future to have the page-in state
	 * structure include a flag (perhaps one for each page in the cluster?)
	 * to be set if a busy PTE was gotten rid of. this would make it so that
	 * operations like unmap would not have to wait on the busy page.
	 */
	kassert(vmp_pte_characterise(state->pte) == kPTEKindBusy);
	vmp_md_pte_create_hw(state->pte, page->pfn, false, true);

	/* release references we took to preserve the pages containing PTEs */
	vmp_page_release_locked(object_state.pgtable_pages[0]);
	vmp_page_release_locked(state->pgtable_pages[0]);

	vmp_release_pfn_lock(kIPLAST);

	/* now release waiters */
	ke_event_signal(&pgstate->event);
	vmp_pager_state_release(pgstate);

	/* this effectively steals the reference we have on the page */
	r = vmp_wsl_insert(vmps, vaddr, true);
	if (r == NIL_WSE) {
		kfatal("Working set insertion failed - evict & unref page!!\n");
	}

	return 0;
}

int
vmp_do_fault(vaddr_t vaddr, bool write)
{
	int r;
	eprocess_t *process;
	vm_procstate_t *vmps;
	struct vmp_pte_wire_state state;
	struct fault_info area_info;
	ipl_t ipl;

	kassert(splget() < kIPLDPC);

	vaddr = PGROUNDDOWN(vaddr);

	if (vaddr >= HHDM_BASE)
		process = kernel_process;
	else
		process = ex_curproc();

#if TRACE_FAULT
	kprintf("Fault at %p (write? %d)\n", vaddr, write);
#endif

	vmps = process->vm;

	if (vaddr < HHDM_BASE ||
	    (vaddr >= KVM_DYNAMIC_BASE &&
		vaddr < KVM_DYNAMIC_BASE + KVM_DYNAMIC_SIZE)) {
		vm_map_entry_t *map_entry;

		ex_rwlock_acquire_read(&vmps->map_lock, "vm_fault: acquire map_lock");
		map_entry = vmp_ps_vad_find(vmps, vaddr);

		if (map_entry == NULL)
			kfatal("VM fault at 0x%zx doesn't have a vad\n", vaddr);

		area_info.object = map_entry->object;
		area_info.writeable = map_entry->flags.protection & kVMWrite;
		area_info.copy = map_entry->flags.cow;
		area_info.offset = map_entry->flags.offset;
		area_info.start = map_entry->start;
		area_info.map_lock_held = true;
	} else if (vaddr >= KVM_UBC_BASE &&
	    vaddr < KVM_UBC_BASE + KVM_UBC_SIZE) {
		/* note: UBC faults are always taken after a window was paged */
		ubc_window_t *window = ubc_addr_to_window(vaddr);

		area_info.object = window->vnode->object;
		area_info.copy = false;
		area_info.writeable = true;
		area_info.offset = window->offset * (UBC_WINDOW_SIZE / PGSIZE);
		area_info.start = ubc_window_addr(window);
		area_info.map_lock_held = false;
	} else {
		kprintf("Page fault in an unacceptable area (0x%zx)\n", vaddr);
		return kVMFaultRetFailure;
	}
	/*
	 * Check if area is nonwriteable and this is a write
	 * fault. If so, signal error.
	 */
	if (write && !area_info.writeable)
		kfatal("Write fault at 0x%zx in nonwriteable vad\n", vaddr);

	area_info.ws_lock_held = true;
	KE_WAIT(&vmps->ws_mutex, false, false, -1);

	ipl = vmp_acquire_pfn_lock();
	r = vmp_wire_pte(process, vaddr, 0, &state, true);
	if (r != kVMFaultRetOK) {
		kfatal("Failed to wire PTE\n");
		/* map mutex unlocked, PFNDB unlocked, and at IPL 0 */
		return r;
	}

	enum vmp_pte_kind pte_kind = vmp_pte_characterise(state.pte);
#ifdef DEBUG_FAULTS
	kprintf("FAULT ADDRESS: 0x%zx PTE KIND: %d\n", vaddr, pte_kind);
#endif

	if (pte_kind == kPTEKindValid &&
	    (!write || vmp_md_hw_pte_is_writeable(state.pte))) {
		kprintf("Nothing to do?\n");
		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(ipl);
	} else if (pte_kind == kPTEKindValid &&
	    !vmp_md_hw_pte_is_writeable(state.pte) && write) {
		pte_t *pte = state.pte;
		vm_page_t *old_page = vmp_pte_hw_page(pte, 1);

		/*
		 * Write fault, VAD permits, PTE valid, PTE not
		 * writeable. Possibilities:
		 * - this page is legally writeable but is not set
		 * writeable because of dirty-bit emulation.
		 * - this is a CoW page
		 */

		if (old_page->use == kPageUseAnonPrivate) {
			/* Dirty fault. */
			vmp_md_pte_create_hw(state.pte,
			    vmp_md_pte_hw_pfn(state.pte, 1), true, true);
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(ipl);
		} else if (area_info.copy) {
			vm_page_t *new_page;
			vm_page_t *pml1_page = state.pgtable_pages[0];

			/* (Symmetric) CoW fault.*/

			r = vmp_page_alloc_locked(&new_page,
			    kPageUseAnonPrivate, false);
			if (r != 0) {
				vmp_pte_wire_state_release(&state, false);
				vmp_release_pfn_lock(ipl);
				ke_mutex_release(&vmps->ws_mutex);
				ex_rwlock_release_read(&vmps->map_lock);
				return kVMFaultRetPageShortage;
			}

			new_page->process = process;
			new_page->referent_pte = V2P((vaddr_t)pte);
			new_page->offset = vaddr / PGSIZE;

			memcpy((void *)vm_page_direct_map_addr(new_page),
			    (void *)vm_page_direct_map_addr(old_page), PGSIZE);

			vmp_page_evict(vmps, pte, pml1_page, vaddr);
			vmp_md_pte_create_hw(pte, new_page->pfn, write, true);
			vmp_pagetable_page_noswap_pte_created(vmps, pml1_page,
			    true);
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(ipl);
		} else {
			kassert(old_page->use == kPageUseFileShared);
			if (!old_page->dirty) {
				/* page is dirty for the first time. */
				old_page->dirty = true;
				if (area_info.object->file.n_dirty_pages++ ==
				    0) {
					/*
					 * first time this increased above 0, so
					 * retain the vnode to keep it alive
					 * until dirty pages have all been
					 * written.
					 */
					kprintf(
					    " -VN- reTAIN Vnode %p has its first dirty page\n",
					    area_info.object->file.vnode);
					/* x-ref vnode dirty refcount */
					vn_retain(area_info.object->file.vnode);
				}
			}
			vmp_md_pte_create_hw(state.pte,
			    vmp_md_pte_hw_pfn(state.pte, 1), true, true);
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(ipl);
		}
	} else if (pte_kind == kPTEKindZero && area_info.object == NULL) {
		vm_page_t *page;
		pte_t *pte = state.pte;
		vm_page_t *pml1_page = state.pgtable_pages[0];

		r = vmp_page_alloc_locked(&page, kPageUseAnonPrivate, false);
		if (r != 0) {
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(ipl);
			ke_mutex_release(&vmps->ws_mutex);
			ex_rwlock_release_read(&vmps->map_lock);
			return kVMFaultRetPageShortage;
		}

		page->process = process;
		page->referent_pte = V2P((vaddr_t)pte);
		page->offset = vaddr / PGSIZE;
		vmp_md_pte_create_hw(pte, page->pfn, write, true);
		vmp_pagetable_page_noswap_pte_created(process->vm, pml1_page, true);
		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(ipl);

		r = vmp_wsl_insert(vmps, vaddr, false);
		if (r == NIL_WSE) {
			/*
			 * we have the working set lock held so we can just
			 * acquire pfn lock, trans the PTE, pml1_page deleted,
			 * unlock again.
			 */
			kfatal("Working set insertion failed - evict!!\n");
		} else {
			page->wsi_hint = r;
			r = 0;
		}
	} else if (pte_kind == kPTEKindZero) {
		r = vmp_do_file_fault(process, vmps, &area_info, &state, vaddr);
		/* pfn lock was released */
	} else if (pte_kind == kPTEKindTrans) {
		vm_page_t *page = vm_pfn_to_page(
		    vmp_md_soft_pte_pfn(state.pte));

		kassert(page->use == kPageUseAnonPrivate);

		vmp_page_retain_locked(page);
		vmp_md_pte_create_hw(state.pte, page->pfn, write, true);
		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(ipl);

		r = vmp_wsl_insert(vmps, vaddr, false);
		if (r == NIL_WSE) {
			/*
			 * we have the working set lock held so we can just
			 * acquire pfn lock, trans the PTE, pml1_page deleted,
			 * unlock again.
			 */
			kfatal("Working set insertion failed - evict!!\n");
		} else {
			page->wsi_hint = r;
			r = 0;
		}
	} else if (pte_kind == kPTEKindSwap) {

		vm_mdl_t *mdl;
		iop_t *iop;
		struct vmp_pager_state *pgstate;
		vm_page_t *page;
		pfn_t drumslot = vmp_md_soft_pte_pfn(state.pte);

		r = vmp_page_alloc_locked(&page, kPageUseAnonPrivate, false);
		if (r != 0) {
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(kIPLAST);
			ke_mutex_release(&vmps->ws_mutex);
			ex_rwlock_release_read(&vmps->map_lock);
			return kVMFaultRetPageShortage;
		}

		pgstate = kmem_xalloc(sizeof(*pgstate), kVMemPFNDBHeld);
		kassert(r == 0);

		pgstate->refcnt = 1;
		pgstate->vpfn = (vaddr / PGSIZE);
		pgstate->length = 1;
		ke_event_init(&pgstate->event, false);

		page->pager_request = pgstate;
		page->owner = process;
		page->dirty = false;
		page->referent_pte = V2P((vaddr_t)state.pte);
		page->offset = vaddr / PGSIZE;
		page->drumslot = drumslot;

		/* create busy PTE */
		vmp_md_pte_create_busy(state.pte, page->pfn);
		vmp_pagetable_page_noswap_pte_created(process->vm,
		    state.pgtable_pages[0], false);

		/* additional retain to keep PTE valid */
		vmp_page_retain_locked(state.pgtable_pages[0]);

		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(kIPLAST);
		ke_mutex_release(&vmps->ws_mutex);
		ex_rwlock_release_read(&vmps->map_lock);
		area_info.map_lock_held = false;

		extern vnode_t *pagefile_vnode;

		mdl = &pgstate->mdl;
		mdl->nentries = 1;
		mdl->offset = 0;
		mdl->write = true;
		mdl->pages[0] = page;
		iop = iop_new_vnode_read(pagefile_vnode, mdl, PGSIZE,
		    drumslot * PGSIZE);

		iop_send_sync(iop);
		iop_free(iop);

		/* only need WS mutex for dealing with a busy page */
		KE_WAIT(&vmps->ws_mutex, false, false, -1);

		vmp_acquire_pfn_lock();

		switch (vmp_pte_characterise(state.pte)) {
		case kPTEKindBusy:
			break;
		default:
			kfatal("Complete this codepath\n");
		}

		kassert(vm_pfn_to_page(vmp_md_soft_pte_pfn(state.pte)) == page);
		vmp_md_pte_create_hw(state.pte, page->pfn, write, true);

		/* release reference we took to preserve the PTE page */
		vmp_page_release_locked(state.pgtable_pages[0]);

		vmp_release_pfn_lock(kIPLAST);

		/* now release waiters */
		ke_event_signal(&pgstate->event);
		vmp_pager_state_release(pgstate);

		/* this effectively steals the reference we have on the page */
		r = vmp_wsl_insert(vmps, vaddr, false);
		if (r == NIL_WSE) {
			/*
			 * we have the working set lock held so we can just
			 * acquire pfn lock, trans the PTE, pml1_page deleted,
			 * unlock again.
			 */
			kfatal("Working set insertion failed - evict!!\n");
		} else {
			page->wsi_hint = r;
			r = 0;
		}
	} else if (pte_kind == kPTEKindBusy) {
		vm_page_t *page = vm_pfn_to_page(
		    vmp_md_soft_pte_pfn(state.pte));
		struct vmp_pager_state *pgstate = page->pager_request;

		vmp_pager_state_retain(pgstate);
		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(kIPLAST);
		ke_mutex_release(&vmps->ws_mutex);
		ex_rwlock_release_read(&vmps->map_lock);


		/* wait for the page-in to complete... */
		KE_WAIT(&pgstate->event, false, false, -1);

		vmp_pager_state_release(pgstate);

		/* and let the collided fault be retried. */
		return kVMFaultRetRetry;
	} else {
		kfatal("Unexpected PTE state %d\n", pte_kind);
	}

	if (area_info.map_lock_held)
		ex_rwlock_release_read(&vmps->map_lock);
	ke_mutex_release(&vmps->ws_mutex);

	return r;
}

void md_intr_frame_trace(md_intr_frame_t *frame);

int
vmp_fault(md_intr_frame_t *frame, vaddr_t vaddr, bool write, vm_page_t **out)
{
	vm_fault_return_t ret;

retry:
	ret = vmp_do_fault(vaddr, write);
	switch (ret) {
	case kVMFaultRetOK:
		break;

	case kVMFaultRetRetry:
		goto retry;

	case kVMFaultRetPageShortage:
		ke_wait(&vmp_page_availability_event, "pagewait", false, false,
		    -1);
		goto retry;

	case kVMFaultRetFailure: {
		md_intr_frame_trace(frame);
		kfatal("Stopping.\n");
	}

	default:
		kfatal("Unexpected fault return code %d\n", ret);
	}

	return ret;
}
