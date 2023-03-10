/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Mar 10 2023.
 */

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/vm.h"
#include "vm/pmapp.h"
#include "vm/vm_internal.h"

/*!
 * @brief Page in an anonymous page in a prototype pagetable.
 * @pre VAD list mutex of \p vmps held
 * @pre Mutex of anonymous/fork object held
 * @pre PFN DB lock held.
 * @pre
 */
vm_fault_return_t
vmp_anonymous_proto_page_in(vm_procstate_t *vmps, ipl_t ipl,
    kmutex_t *obj_mutex, pte_t *proto_pte, pte_t *pte, vaddr_t vaddr)
{
	kwaitstatus_t w;
	vm_page_t *page;
	struct vmp_paging_state *pstate;
	drumslot_t drumslot;
	int ret;

	ret = vmp_page_alloc(vmps, false, kPageUseTransition, &page);
	if (ret != 0) {
		/* low memory, return and let wait */
		vmp_release_pfn_lock(ipl);
		ke_mutex_release(obj_mutex);
		return kVMFaultRetPageShortage;
	}

	pstate = kmem_xalloc(sizeof(struct vmp_paging_state), kVMemPFNDBHeld);
	ke_event_init(&pstate->event, false);

	page->paging_state = pstate;
	drumslot = pte_sw_get_addr(proto_pte);

	/* enter a transition PTE into the fork page */
	pte_transition_enter(proto_pte, page);
	/* ...and into the process page table. */
	pte_transition_enter(pte, page);

	page->refcnt += 2;
	kassert(page->refcnt == 2);

	vmp_release_pfn_lock(ipl);

	/*
	 * there shouldn't be a WSL entry yet. paranoid assert.
	 */
	kassert(vmp_wsl_find(vmps, vaddr) == NULL);
	vmp_wsl_insert(vmps, vaddr);

	ke_mutex_release(obj_mutex);
	ke_mutex_release(&vmps->mutex);

	/* do the actual pagein */
	page_in_anonymous(page, pstate, drumslot);

	w = ke_wait(&vmps->mutex, "vmp_anonymous_proto_page_in: vmps->mutex",
	    false, false, -1);
	kassert(w == kKernWaitStatusOK);
	w = ke_wait(&vmps->mutex, "vmp_anonymous_proto_page_in: obj_mutex",
	    false, false, -1);
	kassert(w == kKernWaitStatusOK);

	/*
	 * because we only establish the prototype PTE in this the first process
	 * to try to fault it in, we can determine whether the page is still in
	 * our working set (and thus our process PTE can be changed) by checking
	 * if the page's refcnt remains 2.
	 *
	 * we (?)still have a pin on both the object page and the PTE
	 */

	ipl = vmp_acquire_pfn_lock();
	if (page->refcnt == 2) {
		/* turn pte into a hardware PTE... */
	}

	page->pageable_use = kPageableUseFork;
	page->use = kPageUseActive;
	// ...
}