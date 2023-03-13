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
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "vm/pmapp.h"
#include "vm/vm_internal.h"

RB_GENERATE(vmp_vpage_rb, vmp_vpage, rb_entry, vmp_vpage_cmp);

intptr_t
vmp_vpage_cmp(struct vmp_vpage *x, struct vmp_vpage *y)
{
	return x->offset - y->offset;
}

/*!
 * Common part of handling a page fault in a VAD backed by a section, for both
 * private (CoW) and shared mappings of a section.
 */
static vm_fault_return_t
fault_section(vm_procstate_t *vmps, vaddr_t vaddr, vm_vad_t *vad,
    struct vmp_section *section, pte_t *pte, vm_fault_flags_t flags,
    bool is_private, vm_page_t **out)
{
	struct vmp_vpage *pageref, key;
	vm_fault_return_t r = kVMFaultRetOK;
	voff_t offset = (vaddr - vad->start) + vad->offset;

	/* first: do we already have a page? */
	key.offset = offset;
	pageref = RB_FIND(vmp_vpage_rb, &section->page_rb, &key);

	if (!pageref) {
		/*
		 * No vpage means the page needs to be read in from the file.
		 * Allocate a backing page, busy it, and insert it into the
		 * section.
		 *
		 * (this logic should go to paging.c, and be unified with
		 * anonymous page logic, with a 'pager' abstraction to do the
		 * actual I/O.)
		 */

		vm_page_t *page;
		int ret;

		ret = vmp_page_alloc(vmps, false, kPageUseActive, &page);
		kassert(ret == kVMFaultRetOK);

		page->use = kPageUseTransition;
		page->pageable_use = kPageableUseSection;
		page->section = &section->hdr;

		pageref = kmem_alloc(sizeof(struct vmp_vpage));
		pageref->offset = offset;
		pte_transition_enter(&pageref->pte, page);

		RB_INSERT(vmp_vpage_rb, &section->page_rb, pageref);

		// ... and our process working set, and a process transition
		// PTE?

		kfatal("Path unimplemented\n");
		// vm_mdl_t *mdl = vm_mdl_alloc(1);
		// mdl->pages[0] = page;
		// iop_t *iop = iop_new_read(section->vnode->vfsp->dev, mdl,
		// 4096,
		//     offset);
		// iop->stack[0].vnode = section->vnode;
		// iop_return_t res = iop_send_sync(iop);
		// kassert(res == kIOPRetCompleted);

		r = kVMFaultRetRetry;
		goto finish;
	} else if (pageref && pte_is_transition(&pageref->pte)) {
		// todo(high): refactor into a common wait-for-transition func?
		/* wait on the event */

		vm_page_t *page;
		struct vmp_paging_state *pstate;

		page = pte_trans_get_page(&pageref->pte);
		vmp_paging_state_retain(page->paging_state);
		pstate = page->paging_state;

		ke_mutex_release(&section->hdr.mutex);
		ke_mutex_release(&vmps->mutex);

		vmp_paging_state_wait(pstate);

		r = kVMFaultRetRetry;
		goto finish;
	} else {
		/* We have the page; map it into the working set list */

		kassert(!pmap_is_present(vmps, vaddr, NULL));

		// ++page refcnt
		pte_hw_enter(pte, pte_hw_get_page(&pageref->pte),
		    is_private ? (kVMRead | kVMExecute) : kVMAll);

		vmp_wsl_insert(vmps, vaddr);
	}

finish:
	if (out && r == kVMFaultRetOK) {
		pte_hw_get_page(&pageref->pte)->refcnt++;
		*out = pte_hw_get_page(&pageref->pte);
	}

	return r;
}

static vm_fault_return_t
fault_private_from_section(vm_procstate_t *vmps, vaddr_t vaddr, vm_vad_t *vad,
    pte_t *pte, vm_fault_flags_t flags, vm_page_t **out)
{
	// TODO: should this lock the section? Maybe need a "don't unlock
	// section on exit" flag for fault_section_sub. but as we are putting a
	// page reference with PTE in our process page tables, this probably is
	// excessive

	struct vmp_section *section = vad->section;
	vm_fault_return_t r;
	vm_page_t *file_page;

	if (vmp_wsl_find(vmps, vaddr) == NULL) {
		/* bring the file's page into the working set read-only */
		r = fault_section(vmps, vaddr, vad, section, pte, flags, true,
		    &file_page);
		if (r == kVMFaultRetRetry)
			return r;

		kassert(r == kVMFaultRetOK);
	} else {
		kassert(pte_is_hw(pte));
		/* grab the page from the working set list */
		file_page = pte_hw_get_page(pte);
		/* we later assume we've got an extra reference */
		file_page->refcnt++;
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
			file_page->refcnt--;

		return kVMFaultRetOK;
	} else {
		/*
		 * but if it's a write fault, we now need to copy the page into
		 * an anon.
		 */

		/*
		 * we know be assured by this point that the page is at least
		 * present in the working set... But now it's necessary to
		 * verify it's not writeable
		 * (could it be? we claim it could below but i'm not sure this
		 * is correct:) which it might be, if we were raced to handle a
		 * fault on the same page.
		 */

		if (pte_hw_is_writeable(pte)) {
			if (*out) {
				kassert(pte_hw_get_page(pte) == file_page);
				*out = file_page;
			} else {
				// assert refcnt remains > 1 after....
				file_page->refcnt--;
			}
			return kVMFaultRetOK;
		}

		int ret;
		vm_page_t *priv_page;

		ret = vmp_page_alloc(vmps, false, kPageUseActive, &priv_page);
		kassert(ret == kVMFaultRetOK);

		vmp_page_copy(file_page, priv_page);

		/* now we can drop our extra reference to file_page */
		file_page->refcnt--;

		/*
		 * we will let the old WSL slot be inherited
		 */
		pte_hw_enter(pte, priv_page, kVMAll);

		/* page has extra refcount from the vmp_page_alloc */
		if (out)
			*out = priv_page;
		else {
			// todo: this is not sufficient if it's the last ref
			priv_page->refcnt--;
		}
	}

	return kVMFaultRetOK;
}

static vm_fault_return_t
fault_private(vm_procstate_t *vmps, vaddr_t vaddr, vm_vad_t *vad, pte_t *pte,
    vm_fault_flags_t flags, vm_page_t **out)
{
	vm_fault_return_t r = kVMFaultRetOK;
	ipl_t ipl;

	/*
	 * We know by this point that if we have a local anonymous page, it
	 * can't have been paged out; that was checked in the master vm_fault().
	 *
	 * We also know in general that we don't have hardware PTEs for pages
	 * not in the working set list.
	 *
	 * So  it could be:
	 * 1. present but not in working set (and hence set to invalid, and not
	 * holding a reference to the underlying page)
	 * 2. present but not writeable (a MAP_PRIVATE page of a section
	 *   object is in the WS; IF)
	 * 3. otherwise either: nonpresent, to be fetched from a backing section
	 * 4 or demand-zero, if there is no backing section.
	 */

	if (pte_is_hw(pte)) {
		vm_page_t *page = pte_hw_get_page(pte);

		kassert(page != NULL);

		if (page->pageable_use == kPageableUseSection) {
			/*
			 * 2. We must have copy-on-write faulted on a
			 * MAP_PRIVATE section page.
			 *
			 * We know that it is in the working set because we
			 * replace HW PTEs for section pages in process page
			 * tables with prototype-denoting PTEs when they leave
			 * the working set.
			 *
			 * So let's confirm that these assumptions are true.
			 */

			kassert(page->section == &vad->section->hdr);
			kassert(vmp_wsl_find(vmps, vaddr) != NULL);
			kassert(flags & kVMFaultWrite);

			/* Now we can allocate the new page and copy. */
			vm_page_t *new_page;
			int ret;

			ipl = vmp_acquire_pfn_lock();
			ret = vmp_page_alloc(vmps, false, kPageUseActive,
			    &new_page);
			if (ret != 0) {
				/* low memory, return and let wait */
				vmp_release_pfn_lock(ipl);
				r = kVMFaultRetPageShortage;
				goto finish;
			}

			// todo: credit shared file page, charge private page

			vmp_page_copy(page, new_page);
			vmp_page_release(page);

			vmp_release_pfn_lock(ipl);

			/*! debug: should have existing  WSL entry */
			kassert(vmp_wsl_find(vmps, vaddr) != NULL);

			new_page->pageable_use = kPageableUseProcessPrivate;
			new_page->proc = vmps;
			new_page->vaddr = vaddr;

			/*
			 * this new page simply inherits the WSL slot.
			 * no retain done because we already have a ref to the
			 * new page.
			 */
			pte_hw_enter(pte, new_page, vad->protection);

			if (out) {
				new_page->refcnt++;
				*out = new_page;
			}
		} else {
			/*
			 * 1. Page is present but shouldn't be in the working
			 * set. So we simply load it back in.
			 */

			/* Test our assumptions. */
			kassert(vmp_wsl_find(vmps, vaddr) == NULL);
			kassert(
			    page->pageable_use == kPageableUseProcessPrivate);

			ipl = vmp_acquire_pfn_lock();
			vmp_activate_page(page);
			vmp_release_pfn_lock(ipl);

			pte_hw_enter(pte, page, vad->protection);
			vmp_wsl_insert(vmps, vaddr);

			if (out) {
				page->refcnt++;
				*out = page;
			}
		}
	} else {
		if (vad->section != NULL) {
			/*
			 * 4. Page is non-present and needs to be demand-zeroed
			 * into existence.
			 */

			vm_page_t *new_page;
			int ret;

			ipl = vmp_acquire_pfn_lock();
			ret = vmp_page_alloc(vmps, false, kPageUseActive,
			    &new_page);
			if (ret != 0) {
				/* low memory, return and let wait */
				vmp_release_pfn_lock(ipl);
				r = kVMFaultRetPageShortage;
				goto finish;
			}

			vmp_release_pfn_lock(ipl);

			// todo: charge private page

			new_page->pageable_use = kPageableUseProcessPrivate;
			new_page->proc = vmps;
			new_page->vaddr = vaddr;

			pte_hw_enter(pte, new_page, vad->protection);
			vmp_wsl_insert(vmps, vaddr);

			if (out) {
				new_page->refcnt++;
				*out = new_page;
			}
		} else {
			/*
			 * 4. Page is non-present and must be fetched from a
			 * backing section.
			 */
			r = fault_private_from_section(vmps, vaddr, vad, pte,
			    flags, out);
		}
	}

finish:
	return r;
}

static vm_fault_return_t
fault_fpage(vm_procstate_t *vmps, vaddr_t vaddr, vm_vad_t *vad,
    struct vmp_forkobj *fobj, struct vmp_forkpage *fpage, pte_t *pte,
    vm_fault_flags_t flags, vm_page_t **out)
{
	ipl_t ipl;
	kwaitstatus_t w;
	vm_fault_return_t r = kVMFaultRetOK;

	w = ke_wait(&fobj->sechdr.mutex, "fault_fpage:fobj->mutex", false,
	    false, -1);
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

			page = pte_hw_get_page(pte);

			/*
			 * replace pte to a private anonymous page. WSL
			 * entry is kept from the read-only mapping.
			 */
			pte_hw_enter(pte, page, vad->protection);
			pmap_invlpg(vaddr);

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
				kfatal("free fork object %p\n", fobj);
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
			vmp_page_release(page);

			vmp_release_pfn_lock(ipl);

			fpage->refcnt--;

			/*! debug: should have existing  WSL entry */
			kassert(vmp_wsl_find(vmps, vaddr) != NULL);
			/*
			 * this new page simply inherits the WSL slot.
			 * no retain done because we already have a ref to the
			 * new page.
			 */
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
			vmp_paging_state_retain(page->paging_state);
			pstate = page->paging_state;

			vmp_release_pfn_lock(ipl);
			ke_mutex_release(&fobj->sechdr.mutex);
			ke_mutex_release(&vmps->mutex);

			vmp_paging_state_wait(pstate);

			return kVMFaultRetRetry;
		} else if (pte_is_outpaged(&fpage->pte)) {
			/* it's paged out. try to page it back in. */

			// vmp_anonymous_proto_page_in....

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
			goto finish;
		} else if (out) {
			page->refcnt++;
			*out = page;
		}
	}

finish:
	ke_mutex_release(&fobj->sechdr.mutex);
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

	w = ke_wait(&vmps->mutex, "vm_fault:vmps->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	vad = vmp_ps_vad_find(vmps, vaddr);
	if (vad == NULL) {
		kdprintf("vm_fault: no or bad VAD at address 0x%lx\n", vaddr);
		return kVMFaultRetFailure;
	}

	/* verify protection permits */
	if (flags & kVMFaultWrite && !(vad->protection & kVMWrite)) {
		kdprintf(
		    "vm_fault: write fault at 0x%lx in non-writeable VAD\n",
		    vaddr);
		return kVMFaultRetFailure;
	}

	pte = pte_get_and_pin(vmps, vaddr);

	if (pte_denotes_proto(pte)) {
		struct vmp_forkpage *fpage;
		struct vmp_forkobj *fobj;

		/*
		 * It's only in the case of fork pages that we actually keep a
		 * PTE pointing to the prototype PTE in our address space at all
		 * times. This is because fork objects point to a densely packed
		 * array of their pages, in ascending order of offset within the
		 * virtual address space they were instantiated from, but with
		 * no easy means to get from a virtual address space in a
		 * process to the fork page.
		 *
		 * We fill in all the fork PTEs at fork-time, and we keep a bit
		 * set in them when we fault one in for read-only use so that we
		 * don't have to unnecessarily traverse our fork objects. So
		 * there will always been an extant fork PTE.
		 *
		 * We can count on fork objects not getting manipulated at any
		 * point.
		 */

		if (flags & kVMFaultPresent) {
			vm_page_t *page;
			/* if it's present, we can get the forkpage from the
			 * vm_page, which holds a backpointer */
			page = vmp_paddr_to_page(pte_hw_get_addr(pte));
			fpage = page->forkpage;
			fobj = (struct vmp_forkobj *)page->section;
			kassert(
			    fobj->sechdr.objhdr.type == kObjTypeSectionFork);
		} else {
			/* else, we extract the compressed pointer */
			fpage = (struct vmp_forkpage *)pte_sw_get_addr(pte);

			/*
			 * have to find the forkobj within whose range fpage is
			 * encompassed
			 */
			struct vmp_forkobj *iter;
			fobj = NULL;

			LIST_FOREACH (iter, &vmps->fork_obj_list, list_entry) {
				if (fpage >= iter->pages &&
				    iter->pages + iter->npages >= fpage) {
					fobj = iter;
					break;
				}
			}

			if (fobj == NULL) {
				kfatal("No fork object found in process"
				       " for fork page\n");
			}
		}

		r = fault_fpage(vmps, vaddr, vad, fobj, fpage, pte, flags, out);
	} else if (pte_is_outpaged(pte)) {
		/*
		 * It's private anonymous memory - bring it back in.
		 */

		ipl_t ipl = vmp_acquire_pfn_lock();
		r = vmp_anonymous_page_in(vmps, vad, ipl, pte, vaddr, out);
	} else if (vad->is_private) {
		/* It's private anonymous memory and it isn't outpaged. */
		r = fault_private(vmps, vaddr, vad, pte, flags, out);
	} else {
		/* It must be a vnode/anonymous object page. */
		kassert(vad->section != NULL);
		r = fault_section(vmps, vaddr, vad, vad->section, pte, flags,
		    false, out);
	}

	ke_mutex_release(&vmps->mutex);

	return r;
}