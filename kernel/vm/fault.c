#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "vm/m68k/vmp_m68k.h"
#include "vmp.h"

extern vm_procstate_t kernel_procstate;
#define CURMAP (&kernel_procstate)

static int vmp_filepage_cmp(struct vmp_filepage *x, struct vmp_filepage *y);

RB_GENERATE(vmp_file_page_tree, vmp_filepage, rb_entry, vmp_filepage_cmp);

static int
vmp_filepage_cmp(struct vmp_filepage *x, struct vmp_filepage *y)
{
	if (x->offset < y->offset)
		return -1;
	else if (x->offset > y->offset)
		return 1;
	else
		return 0;
}

/*!
 * \pre PFN lock held.
 */
static struct vmp_filepage *
section_get_filepage(vm_section_t *section, size_t offset)
{
	struct vmp_filepage key;
	key.offset = offset;
	return RB_FIND(vmp_file_page_tree, &section->file.page_tree, &key);
}

/*!
 * \pre PFN lock held.
 */
static int
do_file_read_fault(struct vmp_pte_wire_state *state, vaddr_t vaddr,
    vm_vad_t *vad, size_t ncluster)
{
	int cluster_idx;
	size_t starting_offset = 0;
	size_t length = 0;
	struct vmp_filepage *found_page;
	bool pages_uncached = false;
	size_t vad_pg_offset = (vaddr - vad->start) / PGSIZE;
	SLIST_HEAD(, vmp_pager_state) pager_state_list;
	TAILQ_HEAD(, iop) iop_list;

	TAILQ_INIT(&iop_list);
	SLIST_INIT(&pager_state_list);

	for (cluster_idx = 0; cluster_idx < ncluster; cluster_idx++) {
		size_t current_offset = cluster_idx;
		found_page = section_get_filepage(vad->section,
		    current_offset + vad_pg_offset + vad->flags.offset);

		if (found_page) {
			if (pages_uncached) {
				kfatal("Implement me!\n");

				/*
				 * reset for the next run of uncached pages
				 */
				pages_uncached = false;
				length = 0;
			}

			/* write found_page into the PTE */
		} else {
			if (!pages_uncached) {
				/*
				 * this is the first of a run of new uncached
				 * pages
				 */
				starting_offset = current_offset;
				pages_uncached = true;
			}
			length++;
		}

		/* try to get next filepage with RB_NEXT as an optimisation */
		if (found_page != NULL) {
			found_page = RB_NEXT(vmp_file_page_tree,
			    vad->section->pages, found_page);
			if (found_page->offset !=
			    current_offset + vad_pg_offset + vad->flags.offset +
				1)
				found_page = NULL;
		}
	}

	if (pages_uncached) {
		/* now handle remaining uncached pages after the loop */
		kprintf("do clustered read (offset: %lld, length: %zu)\n",
		    starting_offset + vad->flags.offset + vad_pg_offset,
		    length);

		/*
		 * found a cached page after a series of
		 * uncached; arrange for read, install the
		 * busy PTEs.
		 */

		vm_mdl_t *mdl;
		iop_t *iop;
		struct vmp_pager_state *pgstate;

		mdl = vm_mdl_alloc_with_pages(length, kPageUseFileShared,
		    &general_account, true);
		pgstate = kmem_alloc(sizeof(*pgstate));
		iop = iop_new_vnode_read(vad->section->file.vnode, mdl,
		    length * PGSIZE,
		    (starting_offset + vad_pg_offset + vad->flags.offset) *
			PGSIZE);

		pgstate->vpfn = (vaddr / PGSIZE) + starting_offset;
		pgstate->length = length;
		ke_event_init(&pgstate->event, false);

		SLIST_INSERT_HEAD(&pager_state_list, pgstate, slist_entry);
		TAILQ_INSERT_HEAD(&iop_list, iop, dev_queue_entry);

		for (int i = 0; i < length; i++) {
			vmp_md_pte_create_busy(state->pte + starting_offset + i,
			    mdl->pages[i]->pfn);
			vmp_md_pagetable_ptes_created(state, length);
			mdl->pages[i]->pager_request = pgstate;
		}
	}

	vmp_release_pfn_lock(kIPLAST);
	ke_mutex_release(&state->vmps->mutex);

	/* post off the IOPs, which we have linked together */
	iop_t *iop, *tmpiop;
	TAILQ_FOREACH_SAFE (iop, &iop_list, dev_queue_entry, tmpiop) {
		iop_send_sync(iop);
		iop_free(iop);
	}

	/* reacquire pertinent locks */
	ke_wait(&state->vmps->mutex, "do_file_read_fault:reacquire vmps->mutex",
	    false, false, 0);
	vmp_acquire_pfn_lock();

	/* iterate through the */
	struct vmp_pager_state *pgstate, *tmppgstate;
	SLIST_FOREACH_SAFE (pgstate, &pager_state_list, slist_entry,
	    tmppgstate) {
		cluster_idx = pgstate->vpfn - (vaddr / PGSIZE);
		pte_t *pte_base = state->pte + cluster_idx;
		for (size_t i = 0; i < pgstate->length; i++) {
			pte_t *pte = pte_base + i;
			vm_page_t *page;
			kassert(pte->sw.kind == kTransition);
			page = vm_paddr_to_page(PFN_TO_PADDR(pte->sw.data));
			kassert(page != NULL);
			kassert(page->pager_request == pgstate);
			vmp_md_pte_create_hw(&pte->hw, pte->sw.data, false);
		}
	}
}

int
vmp_do_fault(struct vmp_pte_wire_state *state, vaddr_t vaddr, bool write)
{
#ifdef __m68k__
	vm_procstate_t *vmps = CURMAP;
	vm_vad_t *vad;
	vm_fault_return_t r;
	ipl_t ipl;
	/* count of pages, including the faulted-on one, to fault in */
	size_t ncluster = 1;

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
	r = vmp_md_wire_pte(vmps, state);
	if (r != kVMFaultRetOK) {
		/* map mutex unlocked, PFNDB unlocked, and at IPL 0 */
		return r;
	}

	/*
	 * Now that the wiring is done, the PFNDB lock remains held.
	 * Let us characterise the PTE, and look for PTEs of the same character
	 * and described by the same VAD (or lack thereof for private anonymous
	 * memory) surrounding it, within the same page of PTEs,
	 * so that we might be able to cluster.
	 */
	{
		size_t maxcluster = (pte_t *)PGROUNDUP((uintptr_t)state->pte) -
		    state->pte;
		maxcluster = MIN2(maxcluster, (vad->end - vaddr) / PGSIZE);
		maxcluster = MIN2(maxcluster, 8);

		/* characterise PTEs.... */
		/* check expansibility of working set.... */

		ncluster = maxcluster;
	}

	/*
	 * the page table containing the PTE is now wired and ready for
	 * inspection.
	 */
	if (vmp_md_pte_is_empty(state->pte)) {
		if (vad->section == NULL) {
			vm_page_t *page;
			int ret;

			ret = vmp_page_alloc_locked(&page, &vmps->account,
			    kPageUseAnonPrivate, false);
			kassert(ret == 0);

			vmp_md_pte_create_hw(state->pte, page->pfn,
			    vad->flags.protection & kVMWrite);
			vmp_wsl_insert(state->vmps, state->addr);
			vmp_md_pagetable_ptes_created(state, 1);
			/*
			 * because PTE was zero, increment refcount of pagetable
			 * page.
			 * we only need to increment the terminal pagetable's
			 * reference and used PTE count;
			 */
		} else {
			do_file_read_fault(state, vaddr, vad, ncluster);
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
#else
	kfatal("VM_FAULT\n");
#endif
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
