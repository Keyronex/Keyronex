#include "kdk/dev.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "ubc.h"
#include "vmp.h"

struct fault_area_info {
	vm_object_t *object;
	vaddr_t start;
	pgoff_t offset;
	bool copy, writeable;
};

static int
vmp_do_file_fault(kprocess_t *process, vm_procstate_t *vmps,
    struct fault_area_info *area_info, struct vmp_pte_wire_state *state,
    vaddr_t vaddr)
{
	vm_object_t *object = area_info->object;
	size_t object_byteoffset = area_info->offset * PGSIZE +
	    (vaddr - area_info->start);
	pgoff_t object_pgoffset = object_byteoffset / PGSIZE;
	struct vmp_pte_wire_state object_state;
	int r;

	r = vmp_wire_pte(process, object_byteoffset, object->vpml4,
	    &object_state);
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

		r = vmp_wsl_insert(vmps, vaddr, false);
		if (r != 0) {
			kfatal("Working set insertion failed - evict!!\n");
		}

		return kVMFaultRetOK;
	}

	case kPTEKindBusy: {
		vm_page_t *page = vm_pfn_to_page(
		    vmp_md_soft_pte_pfn(state->pte));
		struct vmp_pager_state *pgstate = page->pager_request;

		vmp_pte_wire_state_release(&object_state, false);
		vmp_pte_wire_state_release(state, false);
		vmp_release_pfn_lock(kIPLAST);

		/* wait for the page-in to complete... */
		KE_WAIT(&pgstate->event, false, false, -1);
		/*
		 * and reacquire the working set mutex - todo get rid of this
		 * necessity.
		 */
		KE_WAIT(&vmps->mutex, false, false, -1);

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
	kassert(r == 0);

	pgstate = kmem_xalloc(sizeof(*pgstate), kVMemPFNDBHeld);
	kassert(r == 0);

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
	ke_mutex_release(&vmps->mutex);

	mdl = &pgstate->mdl;
	mdl->nentries = 1;
	mdl->offset = 0;
	mdl->pages[0] = page;
	iop = iop_new_vnode_read(object->file.vnode, mdl, PGSIZE,
	    object_byteoffset);

	iop_send_sync(iop);

	KE_WAIT(&vmps->mutex, false, false, -1);
	vmp_acquire_pfn_lock();

	switch (vmp_pte_characterise(object_state.pte)) {
	case kPTEKindZero:
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
		 * we might end up permitting this in the future.
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

	/* this effectively steals the reference we have on the page */
	r = vmp_wsl_insert(vmps, vaddr, false);
	if (r != 0) {
		kfatal("Working set insertion failed - evict & unref page!!\n");
	}

	return r;
}

int
vmp_do_fault(vaddr_t vaddr, bool write)
{
	int r;
	kprocess_t *process;
	vm_procstate_t *vmps;
	struct vmp_pte_wire_state state;
	struct fault_area_info area_info;
	ipl_t ipl;

	kassert(splget() < kIPLDPC);

	if (vaddr >= HHDM_BASE)
		process = &kernel_process;
	else
		process = curproc();

	vmps = process->vm;

	KE_WAIT(&vmps->mutex, false, false, -1);
	if (vaddr < HHDM_BASE ||
	    (vaddr >= KVM_DYNAMIC_BASE &&
		vaddr < KVM_DYNAMIC_BASE + KVM_DYNAMIC_SIZE)) {
		vm_map_entry_t *map_entry = vmp_ps_vad_find(vmps, vaddr);

		if (map_entry == NULL)
			kfatal("VM fault at 0x%zx doesn't have a vad\n", vaddr);

		area_info.object = map_entry->object;
		area_info.writeable = map_entry->flags.protection & kVMWrite;
		area_info.copy = map_entry->flags.cow;
		area_info.offset = map_entry->flags.offset;
		area_info.start = map_entry->start;
	} else if (vaddr >= KVM_UBC_BASE &&
	    vaddr < KVM_UBC_BASE + KVM_UBC_SIZE) {
		/* note: UBC faults are always taken after a window was paged */
		ubc_window_t *window = ubc_addr_to_window(vaddr);

		area_info.object = window->vnode->object;
		area_info.copy = false;
		area_info.writeable = true;
		area_info.offset = window->offset * (UBC_WINDOW_SIZE / PGSIZE);
		area_info.start = ubc_window_addr(window);
	} else {
		kfatal("Page fault in an unacceptable area\n");
	}
	/*
	 * Check if area is nonwriteable and this is a write
	 * fault. If so, signal error.
	 */
	if (write && !area_info.writeable)
		kfatal("Write fault at 0x%zx in nonwriteable vad\n", vaddr);

	ipl = vmp_acquire_pfn_lock();
	r = vmp_wire_pte(process, vaddr, 0, &state);
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
		kfatal("Nothing to do?\n");
	} else if (pte_kind == kPTEKindValid &&
	    !vmp_md_hw_pte_is_writeable(state.pte) && write) {
		/*
		 * Write fault, VAD permits, PTE valid, PTE not
		 * writeable. Possibilities:
		 * - this page is legally writeable but is not set
		 * writeable because of dirty-bit emulation.
		 * - this is a CoW page
		 */
		if (area_info.copy) {
			vm_page_t *new_page, *old_page;
			pte_t *pte = state.pte;
			vm_page_t *pml1_page = state.pgtable_pages[0];

			/* (Symmetric) CoW fault.*/

			r = vmp_page_alloc_locked(&new_page,
			    kPageUseAnonPrivate, false);
			if (r != 0) {
				vmp_pte_wire_state_release(&state, false);
				vmp_release_pfn_lock(ipl);
				ke_mutex_release(&vmps->mutex);
				return kVMFaultRetPageShortage;
			}

			new_page->referent_pte = V2P((vaddr_t)pte);
			old_page = vmp_pte_hw_page(pte, 1);

			memcpy((void *)vm_page_direct_map_addr(new_page),
			    (void *)vm_page_direct_map_addr(old_page), PGSIZE);

			vmp_page_evict(vmps, pte, pml1_page, vaddr);
			vmp_md_pte_create_hw(pte, new_page->pfn, write, true);
			vmp_pagetable_page_noswap_pte_created(vmps, pml1_page,
			    true);
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(ipl);
		} else {
			kfatal("Unhandled write fault\n");
		}
	} else if (pte_kind == kPTEKindZero && area_info.object == NULL) {
		vm_page_t *page;
		pte_t *pte = state.pte;
		vm_page_t *pml1_page = state.pgtable_pages[0];

		r = vmp_page_alloc_locked(&page, kPageUseAnonPrivate, false);
		if (r != 0) {
			vmp_pte_wire_state_release(&state, false);
			vmp_release_pfn_lock(ipl);
			ke_mutex_release(&vmps->mutex);
			return kVMFaultRetPageShortage;
		}

		page->referent_pte = V2P((vaddr_t)pte);
		vmp_md_pte_create_hw(pte, page->pfn, write, true);
		vmp_pagetable_page_noswap_pte_created(process->vm, pml1_page, true);
		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(ipl);

		r = vmp_wsl_insert(vmps, vaddr, false);
		if (r != 0) {
			/*
			 * we have the working set lock held so we can just
			 * acquire pfn lock, trans the PTE, pml1_page deleted,
			 * unlock again.
			 */
			kfatal("Working set insertion failed - evict!!\n");
		}
	} else if (pte_kind == kPTEKindZero) {
		r = vmp_do_file_fault(process, vmps, &area_info, &state, vaddr);
		/* pfn lock was released */
	} else if (pte_kind == kPTEKindTrans) {
		vm_page_t *page = vm_pfn_to_page(
		    vmp_md_soft_pte_pfn(state.pte));

		vmp_page_retain_locked(page);
		vmp_md_pte_create_hw(state.pte, page->pfn, write, true);
		vmp_pte_wire_state_release(&state, false);
		vmp_release_pfn_lock(ipl);

		r = vmp_wsl_insert(vmps, vaddr, false);
		if (r != 0) {
			/*
			 * we have the working set lock held so we can just
			 * acquire pfn lock, trans the PTE, pml1_page deleted,
			 * unlock again.
			 */
			kfatal("Working set insertion failed - evict!!\n");
		}
	} else {
		kfatal("Unexpected PTE state\n");
	}

	ke_mutex_release(&vmps->mutex);

	return r;
}

int
vmp_fault(vaddr_t vaddr, bool write, vm_page_t **out)
{
	vm_fault_return_t ret;
	ret = vmp_do_fault(vaddr, write);
	kassert(ret == kVMFaultRetOK);
	(void)ret;
	return ret;
}
