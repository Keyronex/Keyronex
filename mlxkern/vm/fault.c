/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 17 2023.
 */
/*!
 * @file vm/fault.c
 * @brief Page fault handling.
 *
 * General note: If `out` is specified in any of these functions, the page will
 * be written to there with its reference count incremented.
 */

#include "process/ps.h"
#include "vm/kmem.h"
#include "vm/vm.h"
#include "vm/vm_internal.h"

static void
map_into_wsl(vm_procstate_t *vmps, vaddr_t vaddr, vm_page_t *page,
    vm_protection_t protection)
{
	page->reference_count++;
	pmap_enter(vmps, page->address, vaddr, protection);
}

static vm_fault_return_t
fault_file(vm_procstate_t *vmps, vaddr_t vaddr, vm_section_t *section,
    voff_t offset, vm_fault_flags_t flags, vm_page_t **out)
{
	struct vmp_page_ref *pageref, key;
	vm_page_t *page;

	/* first: do we already have a page? */
	key.page_index = offset / PGSIZE;
	pageref = RB_FIND(vmp_page_ref_rbtree, &section->page_ref_rbtree, &key);

	if (!pageref) {
		/*
		 * Page needs to be read in from the file. Allocate a backing
		 * page, busy it, and insert it into the section.
		 */

		int ret = vi_page_alloc(vmps, false, kPageUseActive, &page);

		kassert(ret == 0);

		page->busy = true;
		page->reference_count = 1;
		page->file = section->file;

		pageref = kmem_alloc(sizeof(struct vmp_page_ref));
		pageref->page_index = offset / PGSIZE;
		pageref->page = page;

		RB_INSERT(vmp_page_ref_rbtree, &section->page_ref_rbtree,
		    pageref);

		/*
		 * Now that that's done, we can drop our locks and initiate the
		 * I/O. Page faults are always at APC level.
		 * (TODO: they aren't yet so this can leave IPL permanently elevated as
		 * the normal IPL drop in vm_fault is absent.)
		 */
		vi_release_pfn_lock(kIPLAPC);
		ke_mutex_release(&vmps->mutex);

		// I/O submission and wait goes here.

		kfatal("Path unimplemented\n");

		return kVMFaultRetRetry;
	}

	kfatal("path unimplemented\n");
}

static vm_fault_return_t
fault_anonymous_from_parent(vm_procstate_t *vmps, vaddr_t vaddr,
    vm_protection_t protection, vm_section_t *section, voff_t offset,
    vm_fault_flags_t flags, vm_page_t **out)
{
	vm_fault_return_t r;
	vm_page_t *file_page;

	kassert(section->parent->kind == kSectionFile);

	/* todo: store wsl slot pointer so we can replace the entry we got */

	if (!(flags & kVMFaultPresent)) {
		/* bring the file's page into the working set read-only */
		r = fault_file(vmps, vaddr, section->parent, offset,
		    flags & ~kVMFaultWrite, &file_page);
		if (r == kVMFaultRetRetry)
			return r;

		kassert(r = kVMFaultRetOK);
	} else {
		/* grab the page from the working set list */
		kfatal("Path unimplemented\n");
	}

	/*
	 * if we made it this far, then no locks were released; we can continue.
	 */
	if (!(flags & kVMFaultWrite)) {
		/* and if it's just a read fault, then the job's done here.
		 * fault_file() will have mapped it into our working set and now
		 * we are free to proceed.
		 */

		if (out)
			*out = file_page;
		else
			/* Refcount already increased by WSL insertion. */
			file_page->reference_count--;

		return 0;
	} else {
		/*
		 * but if it's a write fault, we now need to copy the page into
		 * an anon.
		 */

		int ret;
		vm_page_t *anon_page;
		struct vmp_page_ref *anon_ref;

		ret = vi_page_alloc(vmps, false, kPageUseActive, &anon_page);
		kassert(ret == 0);

		// copy file_page to anon_page
		// replace WSL entry for read-only mapping with write-only
		// mapping

		if (out)
			/* page has a refcount already from the vi_page_alloc */
			*out = anon_page;
		else
			file_page->reference_count--;

		(void)anon_ref;

		kfatal("Path unimplemented\n");
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
	pageref = RB_FIND(vmp_page_ref_rbtree, &section->page_ref_rbtree, &key);

	if (!pageref && section->parent) {
		/*!
		 * if there is no pageref, but there is a parent, we fetch from
		 * there. the parent shall always be a file object.
		 */
		return fault_anonymous_from_parent(vmps, vaddr, protection,
		    section, offset, flags, out);
	}

	kfatal("path unimplemented\n");
}

vm_fault_return_t
vm_fault(vm_procstate_t *vmps, vaddr_t vaddr, vm_fault_flags_t flags,
    vm_page_t **out)
{
	vm_fault_return_t r;
	kwaitstatus_t w;
	vm_vad_t *vad;
	ipl_t ipl;
	voff_t offset;

	w = ke_wait(&vmps->mutex, "vm_fault:vmps->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	vad = vi_ps_vad_find(vmps, vaddr);
	if (!vad) {
		kfatal("vm_fault: no VAD at address 0x%lx\n", vaddr);
	}

	/* verify protection permits */
	if (flags & kVMFaultWrite && !(vad->protection & kVMWrite)) {
		kfatal("vm_fault: write fault at 0x%lx in non-writeable VAD\n",
		    vaddr);
	}

	offset = vaddr - vad->start;

	ipl = vi_acquire_pfn_lock();
	if (vad->section->size <= offset) {
		vi_release_pfn_lock(ipl);
		kfatal("vm_fault: fault past end of object"
		       "(offset 0x%lx, size 0x%lx)\n",
		    offset, vad->section->size);
	}

	if (vad->section->kind == kSectionAnonymous) {
		r = fault_anonymous(vmps, vaddr, vad->protection, vad->section,
		    offset, flags, out);
	} else {
		r = fault_file(vmps, vaddr, vad->section, offset, flags, out);
	}

	if (r != kVMFaultRetOK) {
		/* locks have already been dropped */
		return r;
	}

	vi_release_pfn_lock(ipl);
	ke_mutex_release(&vmps->mutex);

	return kVMFaultRetOK;
}