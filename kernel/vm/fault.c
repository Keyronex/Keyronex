/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 17 2023.
 */
/*!
 * @file vm/fault.c
 * @brief Page fault handling.
 *
 * General note: If `out` is specified in any of these functions, the page will
 * be written to there with its reference count incremented.
 *
 * note: we may want a "wsl_update_protection"? instead of wsl_remove and
 * wsl_insert always. This will let us deal with the case of wired pages, which
 * can't be removed.
 */

#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "vm/pmapp.h"
#include "vm/vm_internal.h"

volatile void *volatile toucher;

#if 0
#include "kdk/devmgr.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vmem.h"

RB_GENERATE(vmp_vpage_rbtree, vmp_page_ref, rbtree_entry, vmp_page_ref_cmp);

int
vmp_page_ref_cmp(struct vmp_page_ref *x, struct vmp_page_ref *y)
{
	return x->page_index - y->page_index;
}

static vm_fault_return_t
fault_file(vm_procstate_t *vmps, vaddr_t vaddr, vm_protection_t protection,
    vm_section_t *section, voff_t offset, vm_fault_flags_t flags,
    vm_page_t **out)
{
	struct vmp_page_ref *pageref, key;

	/* first: do we already have a page? */
	key.page_index = offset / PGSIZE;
	pageref = RB_FIND(vmp_vpage_rbtree, &section->page_ref_rbtree, &key);

	if (!pageref) {
		/*
		 * Page needs to be read in from the file. Allocate a backing
		 * page, busy it, and insert it into the section.
		 */

		vm_page_t *page;
		int ret;

		ret = vmp_page_alloc(vmps, false, kPageUseActive, &page);

		kassert(ret == 0);

		page->busy = true;
		page->vnode = section->vnode;

		pageref = kmem_xalloc(sizeof(struct vmp_page_ref),
		    kVMemPFNDBHeld);
		pageref->page_index = offset / PGSIZE;
		pageref->page = page;

		RB_INSERT(vmp_vpage_rbtree, &section->page_ref_rbtree,
		    pageref);

		/*
		 * Now that that's done, we can drop our locks and initiate the
		 * I/O. Page faults are always at APC level.
		 * (TODO: they aren't yet so this can leave IPL permanently
		 * elevated as the normal IPL drop in vm_fault is absent.)
		 */
		vmp_release_pfn_lock(kIPLAPC);
		ke_mutex_release(&vmps->mutex);

		// I/O submission and wait goes here.

		// We could lock the PFN lock again and check if some 'no longer
		// needed, delete please' flag was set. And if so, we can free
		// that page. Otherwise, do we leave it busy or what? Surely
		// not.
		//
		// We can just put it onto the standby list and refault in the
		// hope that it won't be reused before we fault again. That
		// means we'll need to drop one reference to the page somewhere
		// around here.
		//
		// We can't map it into the address space yet since we don't
		// know if state has changed. What we *might* do is allocate
		// a working set list & page table entry which is invalid but
		// points to the page, just to keep a reference on it.

		// kfatal("Path unimplemented\n");
		vm_mdl_t *mdl = vm_mdl_alloc(1);
		mdl->pages[0] = page;
		iop_t *iop = iop_new_read(section->vnode->vfsp->dev, mdl, 4096,
		    offset);
		iop->stack[0].vnode = section->vnode;
		iop_return_t res = iop_send_sync(iop);
		kassert(res == kIOPRetCompleted);

		return kVMFaultRetRetry;
	}

	/* We have the page; map it into the working set list */
	kassert(!pmap_is_present(vmps, vaddr, NULL));

	vmp_wsl_insert(vmps, vaddr, pageref->page, protection);

	if (out) {
		pageref->page->reference_count++;
		*out = pageref->page;
	}

	return kVMFaultRetOK;
}

/*!
 * @brief Make a new anon and aref and insert into a section.
 */
static void
make_new_anon(vm_section_t *section, voff_t offset, vm_page_t *page)
{
	vm_vpage_t *vpage;
	struct vmp_page_ref *anon_ref;

	vpage = kmem_xalloc(sizeof(vm_vpage_t), kVMemPFNDBHeld);
	vpage->refcount = 1;
	vpage->swapdesc = -1;
	vpage->page = page;

	page->vpage = vpage;

	anon_ref = kmem_xalloc(sizeof(struct vmp_page_ref), kVMemPFNDBHeld);
	anon_ref->page_index = offset / PGSIZE;
	anon_ref->vpage = vpage;

	RB_INSERT(vmp_vpage_rbtree, &section->page_ref_rbtree, anon_ref);
}

static vm_fault_return_t
fault_anonymous_from_parent(vm_procstate_t *vmps, vaddr_t vaddr,
    vm_protection_t protection, vm_section_t *section, voff_t offset,
    vm_fault_flags_t flags, vm_page_t **out)
{
	vm_fault_return_t r;
	vm_page_t *file_page;
	paddr_t paddr;

	kassert(section->parent->kind == kSectionFile);

	if (!pmap_is_present(vmps, vaddr, &paddr)) {
		/* bring the file's page into the working set read-only */
		r = fault_file(vmps, vaddr, protection & ~kVMWrite,
		    section->parent, offset, flags & ~kVMFaultWrite,
		    &file_page);
		if (r == kVMFaultRetRetry)
			return r;

		kassert(r = kVMFaultRetOK);
	} else {
		/* check if page busy here? */
		/* grab the page from the working set list */
		file_page = vmp_paddr_to_page(paddr);
		/* we later assume we've got an extra reference */
		file_page->reference_count++;
	}

	/*
	 * if we made it this far, then no locks were released; we can continue.
	 * we have a pointer to file_page with an extra refcount from
	 * fault_file(). the page was added to our working set list.
	 */
	if (!(flags & kVMFaultWrite)) {
		/* and if it's just a read fault, then the job's done here.
		 * fault_file() will have mapped it into our working set and now
		 * we are free to proceed.
		 */

		/* we've got an extra refcount from fault_file()/*/
		if (out)
			*out = file_page;
		else
			file_page->reference_count--;

		return 0;
	} else {
		/*
		 * but if it's a write fault, we now need to copy the page into
		 * an anon.
		 */

		int ret;
		vm_page_t *anon_page;

		/*
		 * we know be assured by this point that the page is at least
		 * present in the working set.. But now it's necessary to verify
		 * it's not writeable
		 * - which it might be, if we were raced to handle a fault on
		 * the same page.
		 */

		if (pmap_is_writeable(vmps, vaddr, &paddr)) {
			kassert(paddr);
			if (*out) {
				anon_page = vmp_paddr_to_page(paddr);
				anon_page->reference_count++;
				*out = anon_page;
			}
			return kVMFaultRetOK;
		}

		ret = vmp_page_alloc(vmps, false, kPageUseActive, &anon_page);
		kassert(ret == 0);

		vmp_page_copy(file_page, anon_page);

		/* now we can drop our extra reference to file_page */
		file_page->reference_count--;

		/* allocate the vpage and page_ref, and insert */
		make_new_anon(section, offset, anon_page);

		/*
		 * now we can drop the WSL entry for the read-only mapping
		 * (of file_page), and instate the writeable one (of anon_page).
		 */
		vmp_wsl_remove(vmps, vaddr);
		vmp_wsl_insert(vmps, vaddr, anon_page, protection);

		/* page has extra refcount from the vmp_page_alloc */
		if (out)
			*out = anon_page;
		else
			anon_page->reference_count--;
	}

	return kVMFaultRetOK;
}

static vm_fault_return_t
fault_anonymous_vpage(vm_procstate_t *vmps, struct vmp_page_ref *ref,
    vaddr_t vaddr, vm_protection_t protection, vm_section_t *section,
    voff_t offset, vm_fault_flags_t flags, vm_page_t **out)
{
	vm_vpage_t *vpage = ref->vpage;

	if (vpage->page == NULL) {
		/*
		 * Page needs to be read in from the file. Allocate a backing
		 * page, busy it, and associate it with the vpage.
		 */

		vm_page_t *page;
		int ret;

		ret = vmp_page_alloc(vmps, false, kPageUseActive, &page);

		kassert(ret == 0);

		page->busy = true;
		page->vpage = vpage;

		vpage->page = page;

		/*
		 * Now that that's done, we can drop our locks and initiate the
		 * I/O. Page faults are always at APC level.
		 */
		vmp_release_pfn_lock(kIPLAPC);
		ke_mutex_release(&vmps->mutex);

		// I/O submission and wait goes here.
		// see comments in the file_fault()

		kfatal("Path unimplemented\n");

		return kVMFaultRetRetry;
	}

	if (vpage->refcount > 1) {
		if (flags & kVMFaultWrite) {
			vm_vpage_t *newvpage;
			vm_page_t *page;
			int ret;

			ret = vmp_page_alloc(vmps, false, kPageUseActive,
			    &page);
			kassert(ret == 0);

			vmp_page_copy(vpage->page, page);

			newvpage = kmem_alloc(sizeof(vm_vpage_t));
			newvpage->page = page;
			newvpage->refcount = 1;
			newvpage->swapdesc = -1;

			page->vpage = newvpage;

			ref->vpage = newvpage;

			if (pmap_is_present(vmps, vaddr, NULL)) {
				/* existing read-only mapping must go */
				vmp_wsl_remove(vmps, vaddr);
			}

			vmp_wsl_insert(vmps, vaddr, page, protection);

			/* we've got an extra refcount from vmp_page_alloc/*/
			if (out)
				*out = page;
			else
				page->reference_count--;
		} else {
			/* just a read; map existing anon read-only */
			kassert(!pmap_is_present(vmps, vaddr, NULL));
			vmp_wsl_insert(vmps, vaddr, vpage->page,
			    protection & ~kVMWrite);

			if (out) {
				vpage->page->reference_count++;
				*out = vpage->page;
			}
		}
	} else /* refcnt */ {
		if (pmap_is_present(vmps, vaddr, NULL)) {
			/*
			 * the ?only possible case where there's a fault where
			 * page is present and anon has a refcnt of 1 is that it
			 * was marked copy-on-write, and then a virtual copy
			 * took a write fault. so remove the existing mapping
			 * and proceed.
			 */
			vmp_wsl_remove(vmps, vaddr);
		}

		vmp_wsl_insert(vmps, vaddr, vpage->page, protection);

		if (out) {
			vpage->page->reference_count++;
			*out = vpage->page;
		}
	}

	return kVMFaultRetOK;
}

static vm_fault_return_t
fault_anonymous(vm_procstate_t *vmps, vaddr_t vaddr, vm_protection_t protection,
    vm_section_t *section, voff_t offset, vm_fault_flags_t flags,
    vm_page_t **out)
{
	struct vmp_page_ref *pageref, key;

	/* first: do we already have a vpage? */
	key.page_index = offset / PGSIZE;
	pageref = RB_FIND(vmp_vpage_rbtree, &section->page_ref_rbtree, &key);

	if (!pageref && section->parent) {
		/*!
		 * if there is no pageref, but there is a parent, we fetch from
		 * there. the parent shall always be a file object.
		 */
		return fault_anonymous_from_parent(vmps, vaddr, protection,
		    section, offset, flags, out);
	} else if (!pageref) {
		/* no pageref, no parent - demand zero one in. */

		int ret;
		vm_page_t *page;

		ret = vmp_page_alloc(vmps, false, kPageUseActive, &page);
		kassert(ret == 0);

		make_new_anon(section, offset, page);
		vmp_wsl_insert(vmps, vaddr, page, protection);

		/* page has extra refcount from the vmp_page_alloc */
		if (out)
			*out = page;
		else
			page->reference_count--;

		return kVMFaultRetOK;
	} else {
		return fault_anonymous_vpage(vmps, pageref, vaddr, protection,
		    section, offset, flags, out);
	}
}
#endif

static vm_fault_return_t
fault_fpage(vm_procstate_t *vmps, vaddr_t vaddr, vm_vad_t *vad,
    struct vmp_forkobj *fobj, struct vmp_forkpage *fpage, pte_t *pte,
    vm_fault_flags_t flags, vm_page_t **out)
{
	ipl_t ipl;
	kwaitstatus_t w;
	vm_fault_return_t r = kVMFaultRetOK;

	w = ke_wait(&fobj->mutex, "fault_fpage:fobj->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	// some kind of pinning of fpage would be prudent here

	ipl = vmp_acquire_pfn_lock();

	if (flags & kVMFaultPresent) {
		/* if it's present, we *know* that fpage isn't outpaged */

		if (!(flags & kVMFaultWrite)) {
			/*
			 * it's present and this is a read fault
			 * probably being asked to wire page for an MDL.
			 */

			if (out) {
				vm_page_t *page;

				page = pte_hw_get_page(pte);
				page->refcnt++;
				*out = page;
			}
			vmp_release_pfn_lock(ipl);

			/* it can't be outwith memory if it's in a pagetable */
			kassert(pte_is_hw(&fpage->pte));
			/* if it's present, PTE addresses should match */
			kassert(pte_hw_get_addr(pte) ==
			    pte_hw_get_addr(&fpage->pte));
			/*! should have a WSL entry */
			kassert(vmp_wsl_find(vmps, vaddr) != NULL);
		} else if (fpage->refcnt == 1) {
			/*
			 * easy case, we now own it. Turn it into a
			 * regular anonymous mapping and make away with
			 * the prototype.
			 */

			vm_page_t *page;

			// todo: remove PGSIZE from shared accounting, add to
			// private accounting...

			/*
			 * convert the pte to a private anonymous page. WSL
			 * entry is kept from the read-only mapping.
			 */
			pte_hw_set_writeable(pte);
			pte_hw_unset_fork(pte);
			pmap_invlpg(vaddr);

			page = pte_hw_get_page(pte);
			page->pageable_use = kPageableUseProcessPrivate;
			page->proc = vmps;
			if (out) {
				page->refcnt++;
				*out = page;
			}
			vmp_release_pfn_lock(ipl);

			/* can't be outwith memory if it's in a pagetable */
			kassert(pte_is_hw(&fpage->pte));
			/* PTE addresses should match */
			kassert(pte_hw_get_addr(pte) ==
			    pte_hw_get_addr(&fpage->pte));
			/*! debug: should have a WSL entry */
			kassert(vmp_wsl_find(vmps, vaddr) != NULL);

			/* trash the fpage so we see if something goes wrong */
			fpage->pte = 0xDEADBEEFDEADBEEF;
			fpage->offset = 0xDEADDEAD;
			fpage->refcnt = 0xBEEFBEEF;

			// todo: we should have an `fobj_ref` type and decrement
			// number of PTEs we using from it, so we know when to
			// drop the ref.

			if (fobj->npages_referenced-- == 0) {
				// todo: free the fobj
			}
		} else {
			/*
			 * we've copy-on-write faulted and need to duplicate the
			 * page as others still reference it
			 */

			vm_page_t *page, *new_page;
			int ret;

			// todo: remove PGSIZE from shared accounting, add to
			// private accounting...

			ret = vmp_page_alloc(vmps, false, kPageUseActive,
			    &new_page);
			if (ret != 0) {
				/* low memory, return and let wait */
				vmp_release_pfn_lock(ipl);
				r = kVMFaultRetPageShortage;
				goto finish;
			}

			page = pte_hw_get_page(pte);

			vmp_page_copy(page, new_page);

			vmp_release_pfn_lock(ipl);

			vmp_page_release(page);
			fpage->refcnt--;

			/*! debug: should have existing  WSL entry */
			kassert(vmp_wsl_find(vmps, vaddr) != NULL);
			/* this new page simply inherits the WSL slot */
			pte_hw_enter(pte, new_page, vad->protection);

			if (out) {
				new_page->refcnt++;
				*out = new_page;
			}
		}
	} else {
		// todo: can we access fpage safely without PFN DB lock held?
		// probably not, unless we require that fobj lock is acquired
		// before it can get paged out.

		if (pte_is_transition(&fpage->pte)) {
			/* wait on the event */

			vm_page_t *page;
			struct vmp_paging_state *pstate;

			page = pte_trans_get_page(&fpage->pte);
			pstate = vmp_paging_state_retain(page->paging_state);

			vmp_release_pfn_lock(ipl);
			ke_mutex_release(&fobj->mutex);
			ke_mutex_release(&vmps->mutex);

			kfatal("wait on pstate....\n");

			return kVMFaultRetRetry;
		} else if (pte_is_outpaged(&fpage->pte)) {
			/* it's paged out. try to page it back in. */

			vm_page_t *page;
			struct vmp_paging_state *pstate;
			drumslot_t drumslot;
			int ret;

			ret = vmp_page_alloc(vmps, false, kPageUseTransition,
			    &page);
			if (ret != 0) {
				/* low memory, return and let wait */
				vmp_release_pfn_lock(ipl);
				r = kVMFaultRetPageShortage;
				goto finish;
			}

			pstate = kmem_xalloc(sizeof(struct vmp_paging_state),
			    kVMemPFNDBHeld);
			ke_event_init(&pstate->event, false);

			page->paging_state = pstate;
			drumslot = pte_sw_get_addr(&fpage->pte);

			/* enter a transition PTE into the fork page */
			pte_transition_enter(&fpage->pte, page);

			// todo: we need to make sure out pte pin thing actually
			// made sure we do have a PTE for this address.
			// we also really want to be putting in a WSL entry.
			/* ...and into our own page table. */
			pte_transition_enter(pte, page);

			vmp_release_pfn_lock(ipl);

			/*
			 * there shouldn't be a WSL entry yet. paranoid assert.
			 */
			kassert(vmp_wsl_find(vmps, vaddr) == NULL);
			vmp_wsl_insert(vmps, vaddr);

			ke_mutex_release(&fobj->mutex);
			ke_mutex_release(&vmps->mutex);

			page_in_anonymous(page, pstate, drumslot);

			// ... determine if the page is still wanted?
			// what do we do here? unreference the page and just
			// refault?
			//
			// better: we've inserted a WSL entry ; make sure that
			// entry has not been removed (we could adjust WSL
			// removal routine to defer the removal until here if a
			// certain flag is set?)
			//
			// if the entry is still present (meaning that the
			// mapping wasn't removed or anything like that) then we
			// can simply relock and set the proper PTE.

			return kVMFaultRetRetry;
		}

		/*
		 * fpage not present, neither in transition nor outpaged; bring
		 * it in.
		 * we don't bother with the write case here. we let refault
		 * instead.
		 * we don't touch the fpage refcount here because that's
		 * associated with the actual page table entry.
		 */

		vm_page_t *page;
		page = pte_hw_get_page(&fpage->pte);
		vmp_page_retain(page);
		pte_hw_enter(pte, page, vad->protection, /* isFPage = */ true);

		vmp_release_pfn_lock(ipl);

		/*! there shouldn't be a WSL entry yet. paranoid assert. */
		kassert(vmp_wsl_find(vmps, vaddr) == NULL);
		vmp_wsl_insert(vmps, vaddr);

		if (flags & kVMFaultWrite) {
			r = kVMFaultRetRetry;
		}
	}

finish:
	ke_mutex_release(&fobj->mutex);
	return r;
}

vm_fault_return_t
vm_fault(vm_procstate_t *vmps, vaddr_t vaddr, vm_fault_flags_t flags,
    vm_page_t **out)
{
	vm_fault_return_t r = kVMFaultRetOK;
	kwaitstatus_t w;
	vm_vad_t *vad;
	pte_t *pte;
	voff_t offset;

	w = ke_wait(&vmps->mutex, "vm_fault:vmps->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	vad = vmp_ps_vad_find(vmps, vaddr);
	if (vad == NULL) {
		kdprintf("vm_fault: no or bad VAD at address 0x%lx\n", vaddr);
		return kVMFaultRetFailure;
	}

	/* verify protection permits */
	if (flags & kVMFaultWrite && !(vad->protection & kVMWrite)) {
		kfatal("vm_fault: write fault at 0x%lx in non-writeable VAD\n",
		    vaddr);
	}

	pte = pte_get_and_pin(vmps, vaddr);

	if (pte && pte_is_fork(pte)) {
		struct vmp_forkpage *fpage;
		struct vmp_forkobj *fobj;

		/*
		 * We fill in all the fork PTEs at fork-time, and we keep a bit
		 * set in them when we fault one in for read-only use so that we
		 * don't have to unnecessarily traverse our fork objects. So
		 * there will always been an extant fork PTE.
		 */

		if (flags & kVMFaultPresent) {
			vm_page_t *page;
			/* if it's present, we can get the forkpage from the
			 * vm_page, which holds a backpointer */
			page = vmp_paddr_to_page(pte_hw_get_addr(pte));
			fpage = page->forkpage;
		} else {
			/* else, we extract the compressed pointer */
			fpage = (struct vmp_forkpage *)pte_sw_get_addr(pte);
		}

		r = fault_fpage(vmps, vaddr, vad, fobj, fpage, pte, flags, out);
	} else {
	}

	ke_mutex_release(&vmps->mutex);

	return r;
}