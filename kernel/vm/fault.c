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

#include "kdk/devmgr.h"
#include "kdk/kmem.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vmem.h"
#include "vm/vm_internal.h"

RB_GENERATE(vmp_objpage_rbtree, vmp_objpage, rbtree_entry, vmp_objpage_cmp);

int
vmp_objpage_cmp(struct vmp_objpage *x, struct vmp_objpage *y)
{
	return x->page_index - y->page_index;
}

static vm_fault_return_t
fault_file(vm_map_t *map, vaddr_t vaddr, vm_protection_t protection,
    vm_object_t *section, voff_t offset, vm_fault_flags_t flags,
    vm_page_t **out)
{
	struct vmp_objpage *pageref, key;

	/* first: do we already have a page? */
	key.page_index = offset / PGSIZE;
	pageref = RB_FIND(vmp_objpage_rbtree, &section->page_ref_rbtree, &key);

	if (!pageref) {
		/*
		 * Page needs to be read in from the file. Allocate a backing
		 * page, busy it, and insert it into the section.
		 */

		vm_page_t *page;
		int ret;

		ret = vmp_page_alloc(map, false, kPageUseActive, &page);

		kassert(ret == 0);

		page->busy = true;
		page->vnode = section->vnode;

		pageref = kmem_xalloc(sizeof(struct vmp_objpage),
		    kVMemPFNDBHeld);
		pageref->page_index = offset / PGSIZE;
		pageref->page = page;

		RB_INSERT(vmp_objpage_rbtree, &section->page_ref_rbtree,
		    pageref);

		/*
		 * Now that that's done, we can drop our locks and initiate the
		 * I/O. Page faults are always at APC level.
		 * (TODO: they aren't yet so this can leave IPL permanently
		 * elevated as the normal IPL drop in vm_fault is absent.)
		 */
		vmp_release_pfn_lock(kIPLAPC);
		ke_mutex_release(&map->mutex);

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
	kassert(!pmap_is_present(map, vaddr, NULL));

	vmp_wsl_insert(map, vaddr, pageref->page, protection);

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
make_new_anon(vm_object_t *section, voff_t offset, vm_page_t *page)
{
	vm_vpage_t *vpage;
	struct vmp_objpage *anon_ref;

	vpage = kmem_xalloc(sizeof(vm_vpage_t), kVMemPFNDBHeld);
	vpage->refcount = 1;
	vpage->swapdesc = -1;
	vpage->page = page;

	page->vpage = vpage;

	anon_ref = kmem_xalloc(sizeof(struct vmp_objpage), kVMemPFNDBHeld);
	anon_ref->page_index = offset / PGSIZE;
	anon_ref->vpage = vpage;

	RB_INSERT(vmp_objpage_rbtree, &section->page_ref_rbtree, anon_ref);
}

static vm_fault_return_t
fault_anonymous_from_parent(vm_map_t *map, vaddr_t vaddr,
    vm_protection_t protection, vm_object_t *section, voff_t offset,
    vm_fault_flags_t flags, vm_page_t **out)
{
	vm_fault_return_t r;
	vm_page_t *file_page;
	paddr_t paddr;

	kassert(section->parent->kind == kSectionFile);

	if (!pmap_is_present(map, vaddr, &paddr)) {
		/* bring the file's page into the working set read-only */
		r = fault_file(map, vaddr, protection & ~kVMWrite,
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

		if (pmap_is_writeable(map, vaddr, &paddr)) {
			kassert(paddr);
			if (*out) {
				anon_page = vmp_paddr_to_page(paddr);
				anon_page->reference_count++;
				*out = anon_page;
			}
			return kVMFaultRetOK;
		}

		ret = vmp_page_alloc(map, false, kPageUseActive, &anon_page);
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
		vmp_wsl_remove(map, vaddr);
		vmp_wsl_insert(map, vaddr, anon_page, protection);

		/* page has extra refcount from the vmp_page_alloc */
		if (out)
			*out = anon_page;
		else
			anon_page->reference_count--;
	}

	return kVMFaultRetOK;
}

static vm_fault_return_t
fault_anonymous_vpage(vm_map_t *map, struct vmp_objpage *ref,
    vaddr_t vaddr, vm_protection_t protection, vm_object_t *section,
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

		ret = vmp_page_alloc(map, false, kPageUseActive, &page);

		kassert(ret == 0);

		page->busy = true;
		page->vpage = vpage;

		vpage->page = page;

		/*
		 * Now that that's done, we can drop our locks and initiate the
		 * I/O. Page faults are always at APC level.
		 */
		vmp_release_pfn_lock(kIPLAPC);
		ke_mutex_release(&map->mutex);

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

			ret = vmp_page_alloc(map, false, kPageUseActive,
			    &page);
			kassert(ret == 0);

			vmp_page_copy(vpage->page, page);

			newvpage = kmem_alloc(sizeof(vm_vpage_t));
			newvpage->page = page;
			newvpage->refcount = 1;
			newvpage->swapdesc = -1;

			page->vpage = newvpage;

			ref->vpage = newvpage;

			if (pmap_is_present(map, vaddr, NULL)) {
				/* existing read-only mapping must go */
				vmp_wsl_remove(map, vaddr);
			}

			vmp_wsl_insert(map, vaddr, page, protection);

			/* we've got an extra refcount from vmp_page_alloc/*/
			if (out)
				*out = page;
			else
				page->reference_count--;
		} else {
			/* just a read; map existing anon read-only */
			kassert(!pmap_is_present(map, vaddr, NULL));
			vmp_wsl_insert(map, vaddr, vpage->page,
			    protection & ~kVMWrite);

			if (out) {
				vpage->page->reference_count++;
				*out = vpage->page;
			}
		}
	} else /* refcnt */ {
		if (pmap_is_present(map, vaddr, NULL)) {
			/*
			 * the ?only possible case where there's a fault where
			 * page is present and anon has a refcnt of 1 is that it
			 * was marked copy-on-write, and then a virtual copy
			 * took a write fault. so remove the existing mapping
			 * and proceed.
			 */
			vmp_wsl_remove(map, vaddr);
		}

		vmp_wsl_insert(map, vaddr, vpage->page, protection);

		if (out) {
			vpage->page->reference_count++;
			*out = vpage->page;
		}
	}

	return kVMFaultRetOK;
}

static vm_fault_return_t
fault_anonymous(vm_map_t *map, vaddr_t vaddr, vm_protection_t protection,
    vm_object_t *section, voff_t offset, vm_fault_flags_t flags,
    vm_page_t **out)
{
	struct vmp_objpage *pageref, key;

	/* first: do we already have a vpage? */
	key.page_index = offset / PGSIZE;
	pageref = RB_FIND(vmp_objpage_rbtree, &section->page_ref_rbtree, &key);

	if (!pageref && section->parent) {
		/*!
		 * if there is no pageref, but there is a parent, we fetch from
		 * there. the parent shall always be a file object.
		 */
		return fault_anonymous_from_parent(map, vaddr, protection,
		    section, offset, flags, out);
	} else if (!pageref) {
		/* no pageref, no parent - demand zero one in. */

		int ret;
		vm_page_t *page;

		ret = vmp_page_alloc(map, false, kPageUseActive, &page);
		kassert(ret == 0);

		make_new_anon(section, offset, page);
		vmp_wsl_insert(map, vaddr, page, protection);

		/* page has extra refcount from the vmp_page_alloc */
		if (out)
			*out = page;
		else
			page->reference_count--;

		return kVMFaultRetOK;
	} else {
		return fault_anonymous_vpage(map, pageref, vaddr, protection,
		    section, offset, flags, out);
	}
}

vm_fault_return_t
vm_fault(vm_map_t *map, vaddr_t vaddr, vm_fault_flags_t flags,
    vm_page_t **out)
{
	vm_fault_return_t r;
	kwaitstatus_t w;
	vm_map_entry_t *vad;
	ipl_t ipl;
	voff_t offset;

	w = ke_wait(&map->mutex, "vm_fault:map->mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	vad = vmp_map_find(map, vaddr);
	if (vad == NULL || vad->section == NULL) {
		kdprintf("vm_fault: no or bad VAD at address 0x%lx\n", vaddr);
		return kVMFaultRetFailure;
	}

	/* verify protection permits */
	if (flags & kVMFaultWrite && !(vad->protection & kVMWrite)) {
		kfatal("vm_fault: write fault at 0x%lx in non-writeable VAD\n",
		    vaddr);
	}

	offset = vaddr - vad->start;

	ipl = vmp_acquire_pfn_lock();
	if (vad->section->size <= offset) {
		vmp_release_pfn_lock(ipl);
		kfatal("vm_fault: fault past end of object"
		       "(offset 0x%lx, size 0x%lx)\n",
		    offset, vad->section->size);
	}

	if (vad->section->kind == kSectionAnonymous) {
		r = fault_anonymous(map, vaddr, vad->protection, vad->section,
		    offset, flags, out);
	} else {
		r = fault_file(map, vaddr, vad->protection, vad->section,
		    offset, flags, out);
	}

	if (r != kVMFaultRetOK) {
		/* locks have already been dropped */
		return r;
	}

	vmp_release_pfn_lock(ipl);
	ke_mutex_release(&map->mutex);

	return kVMFaultRetOK;
}