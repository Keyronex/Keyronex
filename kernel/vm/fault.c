#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "vmp.h"

extern vm_procstate_t kernel_procstate;
#define CURMAP (&kernel_procstate)

int
vmp_do_fault(struct vmp_pte_wire_state *state, vaddr_t vaddr, bool write)
{
#if 0
	vm_procstate_t *vmps = CURMAP;
	vm_vad_t *vad;
	vm_fault_return_t r;
	ipl_t ipl;

	kassert(splget() < kIPLDPC);

	KE_WAIT(&vmps->mutex, false, false, -1);
	vad = vmp_ps_vad_find(vmps, vaddr);

	if (!vad) {
		kfatal("VM fault at 0x%zx doesn't have a vad\n", vaddr);
	}

	/*
	 * a VAD exists. Check if it is nonwriteable and this is a write fault.
	 * If so, signal error.
	 */
	if (write && !(vad->flags.protection & kVMWrite))
		kfatal("Write fault at 0x%zx in nonwriteable vad\n", vaddr);

	ipl = vmp_acquire_pfn_lock();
	r = vmp_md_wire_pte(state);
	if (r != kVMFaultRetOK) {
		/* map mutex unlocked, PFNDB unlocked, and at IPL 0 */
		return r;
	}

	/*
	 * the page table containing the PTE is now wired and ready for
	 * inspection.
	 */
	if (vmp_md_pte_is_empty(state->pte)) {
		if (vad->section == NULL) {
			vm_page_t *page;
			int ret;

			ret = vmp_page_alloc_locked(&page, false);
			kassert(ret == 0);

			vmp_md_pte_create_hw(state->pte, page->pfn, vad->flags.protection & kVMWrite);
			vmp_wsl_insert(state->vmps, state->addr);
			vmp_md_used_pagetable(state);
			/*
			 * because PTE was zero, increment refcount of pagetable
			 * page.
			 * we only need to increment the terminal pagetable's
			 * reference and used PTE count;
			 */
		} else {
			kfatal("demand page\n");
		}
	} else if (vmp_md_pte_is_valid(state->pte) && write &&
	    !vmp_md_hw_pte_is_writeable(state->pte)) {
		/*
		 * Write fault, VAD permits, PTE valid, PTE not writeable.
		 * Possibilities:
		 * - this page is legally writeable but is not set writeable
		 *   because of dirty-bit emulation.
		 * - this is a CoW page
		 */
		kfatal("write, vad permits, valid, pte not writeable");
	} else {
		kfatal("unhandled case\n");
	}

	vmp_release_pfn_lock(ipl);
	ke_mutex_release(&vmps->mutex);

	return kVMFaultRetOK;
#endif
	kfatal("VM_FAULT\n");
}

int
vmp_fault(vaddr_t vaddr, bool write, vm_account_t *out_account, vm_page_t **out)
{
	struct vmp_pte_wire_state state;

	state.addr = vaddr;
	state.pte = NULL;
	state.vmps = CURMAP;
	memset(state.pgtable_pages, 0x0, sizeof(state.pgtable_pages));

	vmp_do_fault(&state, vaddr, write);
}
