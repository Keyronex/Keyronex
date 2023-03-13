/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Mar 10 2023.
 */

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/vm.h"
#include "vm/pmapp.h"
#include "vm/vm_internal.h"

static int
page_in_anonymous(vm_page_t *page, struct vmp_paging_state *state,
    drumslot_t drumslot)
{
	kfatal("Page_in_anonymous\n");

	ke_event_signal(&state->event);
	return -1;
}

/*!
 * @brief Page in an anonymous page in a prototype pagetable.
 *
 * @pre VAD list mutex of \p vmps held
 * @pre Mutex of \p sect held
 * @pre PFN DB lock held.
 * @pre \p pte and \p proto_pte pinned in-memory
 *
 * @post VAD list mutex held again
 * @post Mutex of \p sect held again
 * @post PFN DB lock NOT held
 * @post pte and proto_pte remain pinned
 */
vm_fault_return_t
vmp_anonymous_proto_page_in(vm_procstate_t *vmps, vm_vad_t *vad,
    struct vmp_section *sect, ipl_t ipl, pte_t *proto_pte, pte_t *pte,
    vaddr_t vaddr, vm_page_t **out)
{
	kwaitstatus_t w;
	vm_page_t *page;
	struct vmp_paging_state *pstate;
	drumslot_t drumslot;
	int ret;
	vm_fault_return_t r;

	ret = vmp_page_alloc(vmps, false, kPageUseTransition, &page);
	if (ret != 0) {
		/* low memory, return and let wait */
		vmp_release_pfn_lock(ipl);
		ke_mutex_release(&sect->hdr.mutex);
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

	/* retain the section so it won't go away on us */
	obj_direct_retain(sect);

	ke_mutex_release(&sect->hdr.mutex);
	ke_mutex_release(&vmps->mutex);

	/* do the actual pagein */
	page_in_anonymous(page, pstate, drumslot);

	w = ke_wait(&vmps->mutex, "vmp_anonymous_proto_page_in: vmps->mutex",
	    false, false, -1);
	kassert(w == kKernWaitStatusOK);
	w = ke_wait(&sect->hdr.mutex,
	    "vmp_anonymous_proto_page_in: sect->mutex", false, false, -1);
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
		/*
		 * turn pte into a hardware PTE... what if the VAD changed
		 * though, e.g. protection lowered? perhaps
		 */
		r = kVMFaultRetOK;
	} else /* mappings changed */
		r = kVMFaultRetRetry;

	/*
	 * turn proto_pte into a hardware PTE...
	 */

	page->pageable_use = kPageableUseSection;
	page->use = kPageUseActive;
	if (sect->hdr.objhdr.type == kObjTypeSectionAnon ||
	    sect->hdr.objhdr.type == kObjTypeVNode) {
		page->vpage = __containerof(proto_pte, vmp_vpage_t, pte);
	} else {
		kassert(sect->hdr.objhdr.type == kObjTypeSectionFork);
		page->forkpage = __containerof(proto_pte, struct vmp_forkpage,
		    pte);
	}

	if (r == kVMFaultRetOK && out) {
		*out = page;
	} else
		vmp_page_release(page);

	return r;
}

/*!
 * @brief Page in an anonymous page in a process pagetable.
 * @pre VAD list mutex of \p vmps held
 * @pre PFN DB lock held.
 * @pre \p pte pinned in-memory
 *
 * @post VAD list mutex held again
 * @post PFN DB lock NOT held
 * @post pte remains pinned
 */
vm_fault_return_t
vmp_anonymous_page_in(vm_procstate_t *vmps, vm_vad_t *vad, ipl_t ipl,
    pte_t *pte, vaddr_t vaddr, vm_page_t **out)
{
	kwaitstatus_t w;
	vm_page_t *page;
	struct vmp_paging_state *pstate;
	drumslot_t drumslot;
	int ret;
	vm_fault_return_t r;

	ret = vmp_page_alloc(vmps, false, kPageUseTransition, &page);
	if (ret != 0) {
		/* low memory, return and let wait */
		vmp_release_pfn_lock(ipl);
		return kVMFaultRetPageShortage;
	}

	pstate = kmem_xalloc(sizeof(struct vmp_paging_state), kVMemPFNDBHeld);
	ke_event_init(&pstate->event, false);

	page->paging_state = pstate;
	drumslot = pte_sw_get_addr(pte);

	/* enter a transition PTE into the process */
	pte_transition_enter(pte, page);

	page->refcnt += 2;
	kassert(page->refcnt == 2);

	vmp_release_pfn_lock(ipl);

	/*
	 * there shouldn't be a WSL entry yet. paranoid assert.
	 */
	kassert(vmp_wsl_find(vmps, vaddr) == NULL);
	vmp_wsl_insert(vmps, vaddr);

	ke_mutex_release(&vmps->mutex);

	/* do the actual pagein */
	page_in_anonymous(page, pstate, drumslot);

	w = ke_wait(&vmps->mutex, "vmp_anonymous_page_in: vmps->mutex", false,
	    false, -1);
	kassert(w == kKernWaitStatusOK);
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
		/*
		 * turn pte into a hardware PTE... what if the VAD changed
		 * though, e.g. protection lowered? perhaps
		 */
		r = kVMFaultRetOK;
	} else /* mappings changed */
		r = kVMFaultRetRetry;

	page->pageable_use = kPageableUseProcessPrivate;
	page->use = kPageUseActive;
	page->proc = vmps;
	page->vaddr = vaddr;

	if (r == kVMFaultRetOK && out) {
		*out = page;
	} else
		vmp_page_release(page);

	return r;
}