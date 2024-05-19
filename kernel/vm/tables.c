#include "kdk/dev.h"
#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "nanokern/ki.h"
#include "vmp.h"

#define PAGETABLE_PAGING 1

extern vm_procstate_t kernel_procstate;

static bool
page_is_root_table(vm_page_t *page)
{
	return (page->use == (kPageUsePML1 + (VMP_TABLE_LEVELS - 1))) ||
	    page->use == kPageUseVPML4;
}

static bool
use_is_hw_table(enum vm_page_use use)
{
	return use >= kPageUsePML1 &&
	    use <= kPageUsePML1 + VMP_TABLE_LEVELS - 1;
}

/*!
 * @brief Pagetable page is retained, and nonzero and valid PTEs incremented
 */
void
vmp_pagetable_page_noswap_pte_created(vm_procstate_t *ps, vm_page_t *page,
    bool is_new)
{
	kassert(ke_spinlock_held(&vmp_pfn_lock));

	vmp_page_retain_locked(page);
	if (is_new)
		page->nonzero_ptes++;
	page->noswap_ptes++;
#if PAGETABLE_PAGING
	if (page->noswap_ptes == 1 && !page_is_root_table(page)) {
		/*
		 * do nothing?
		 * page should already be pointed to validly by vmp_wire_pte
		 */
	}
#endif
}

/*!
 * @brief Update pagetable page after PTE(s) made zero within it.
 *
 * This will amend the PFNDB entry's nonswap and nonzero PTE count, and if the
 * new nonzero PTE count is zero, delete the page. If the new nonswap PTE count
 * is zero, the page will be unlocked from its owning process' working set.
 *
 * @pre ps->ws_lock held
 */
void
vmp_pagetable_page_pte_deleted(vm_procstate_t *ps, vm_page_t *page,
    bool was_swap)
{
	kassert(ke_spinlock_held(&vmp_pfn_lock));

	if (page->nonzero_ptes-- == 1 && !page_is_root_table(page)) {
		vm_page_t *dirpage;
		enum vm_page_use use = page->use;

		vmp_page_delete_locked(page);

		kassert(page->referent_pte != 0);
		dirpage = vm_paddr_to_page(page->referent_pte);
		if (use_is_hw_table(use))
			vmp_md_delete_table_pointers(ps, dirpage,
			    (pte_t *)P2V(page->referent_pte));
		else {
			((pte_t *)P2V(page->referent_pte))->value = 0x0;
			vmp_pagetable_page_pte_deleted(ps, dirpage, false);
		}

		page->noswap_ptes = 0;
		page->referent_pte = 0;

#if EXTREME_SANITY_CHECKS
		for (int i = 0; i < PGSIZE / sizeof(pte_t); i++) {
			pte_t *pte = (pte_t *)vm_page_direct_map_addr(page);
			kassert(pte[i].value == 0x0);
		}
#endif

		/*! and once for the PTE zeroing; this will free the page. */
		vmp_page_release_locked(page);

		return;
	}
	if (!was_swap && page->noswap_ptes-- == 1 && !page_is_root_table(page)) {
#if PAGETABLE_PAGING
		vm_page_t *dirpage = vm_paddr_to_page(page->referent_pte);

		if (use_is_hw_table(page->use))
			vmp_md_trans_table_pointers(ps, dirpage,
			    (pte_t *)P2V(page->referent_pte), page);
		else
			kfatal("Implement me?\n");
#endif
	}
	if (!was_swap) {
		page->dirty = true;
		vmp_page_release_locked(page);
	}
}

void
vmp_pagetable_page_pte_became_swap(vm_procstate_t *ps, vm_page_t *page)
{
#if PAGETABLE_PAGING
	if (page->noswap_ptes-- == 1 && !page_is_root_table(page)) {
		vm_page_t *dirpage;

		kassert(page->referent_pte != 0);
		dirpage = vm_paddr_to_page(page->referent_pte);

		if (use_is_hw_table(page->use))
			vmp_md_trans_table_pointers(ps, dirpage,
			    (pte_t *)P2V(page->referent_pte), page);
		else
			kfatal("Implement me?\n");
	}
	page->dirty = true;
	vmp_page_release_locked(page);
#else
	page->noswap_ptes--;
#endif
}

void
vmp_pte_wire_state_release(struct vmp_pte_wire_state *state, bool prototype)
{
	kassert(ke_spinlock_held(&vmp_pfn_lock));

	for (int i = 0; i < (prototype ? 4 : VMP_TABLE_LEVELS); i++) {
		if (state->pgtable_pages[i] == NULL)
			continue;
		vmp_pagetable_page_pte_deleted(prototype ?
			&kernel_procstate :
			state->pgtable_pages[i]->process->vm,
		    state->pgtable_pages[i], false);
	}
}

static void
setup_pte_counts(vm_page_t *page)
{
	pte_t *pte = (pte_t *)vm_page_direct_map_addr(page);

	for (size_t i = 0; i < PGSIZE / sizeof(pte_t); i++, pte++) {
		switch (vmp_pte_characterise(pte)) {
		case kPTEKindSwap:
			page->nonzero_ptes++;

		case kPTEKindZero:
			break;

		default:
			kfatal("Unexpected PTE kind in paged-in page!\n");
		}
	}
}

/*!
 * \pre Working set mutex held
 * \pre PFN database lock held
 */
int
vmp_wire_pte(eprocess_t *ps, vaddr_t vaddr, paddr_t prototype,
    struct vmp_pte_wire_state *state, bool create)
{
	int indexes[4 + 1];
	vm_page_t *pages[4] = { 0 };
	int nlevels = prototype == 0 ? VMP_TABLE_LEVELS : 4;
	pte_t *table;

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	if (prototype == 0)
		vmp_addr_unpack(vaddr, indexes);
	else {
		uint64_t virta = (uintptr_t)vaddr;
		indexes[4] = ((virta >> 39) & 0x1FF);
		indexes[3] = ((virta >> 30) & 0x1FF);
		indexes[2] = ((virta >> 21) & 0x1FF);
		indexes[1] = ((virta >> 12) & 0x1FF);
		indexes[0] = 0;
	}

	/*
	 * start by pinning root table with a valid-pte reference, to keep it
	 * locked in the working set. this same approach is used through the
	 * function.
	 *
	 * the principle is that at each iteration, the page table we are
	 * examining has been locked into the working set by the processing of
	 * the prior level. as such, pin the root table by calling the
	 * new-nonswap-pte function; this pins the page.
	 */
	if (prototype == 0) {
		table = (pte_t *)P2V(ps->vm->md.table);
		pages[nlevels - 1] = vm_paddr_to_page(ps->vm->md.table);
	} else {
		table = (pte_t *)P2V(prototype);
		pages[nlevels - 1] = vm_paddr_to_page(prototype);
	}

	vmp_pagetable_page_noswap_pte_created(ps->vm, pages[nlevels - 1], true);

	for (int level = nlevels; level > 0; level--) {
		pte_t *pte = &table[indexes[level]];
		int pte_level = prototype == 0 ? level : 1;

		/* note - level is 1-based */

		if (level == 1) {
			memcpy(state->pgtable_pages, pages, sizeof(pages));
			state->pte = pte;
			return 0;
		}

	restart_level:
		switch (vmp_pte_characterise(pte)) {
		case kPTEKindValid: {
			vm_page_t *page = vmp_pte_hw_page(pte, pte_level);
			pages[level - 2] = page;
			/* pin this next level */
			vmp_pagetable_page_noswap_pte_created(ps->vm, page,
			    true);
			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, pte_level));
			break;
		}

		case kPTEKindBusy: {
			pfn_t pfn = vmp_md_soft_pte_pfn(pte);
			vm_page_t *page = vm_pfn_to_page(pfn);
			struct vmp_pager_state *pgstate = page->pager_request;

			vmp_pager_state_retain(pgstate);
			vmp_release_pfn_lock(kIPLAST);
			ke_mutex_release(&ps->vm->mutex);

			KE_WAIT(&pgstate->event, false, false, -1);
			vmp_pager_state_release(pgstate);

			KE_WAIT(&ps->vm->mutex, false, false, -1);
			vmp_acquire_pfn_lock();
			goto restart_level;
		}

		case kPTEKindTrans: {
			pfn_t pfn = vmp_md_soft_pte_pfn(pte);
			vm_page_t *page = vm_pfn_to_page(pfn);
			/* retain for our wiring purposes */
			pages[level - 2] = page;

			kassert(page->use != kPageUseFree);

			/* manually adjust the page */
			vmp_page_retain_locked(page);
			vmp_pagetable_page_noswap_pte_created(ps->vm, page,
			    true);

			vmp_md_setup_table_pointers(ps->vm, pages[level - 1],
			    page, pte, kWasTrans);

			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
			break;
		}

		case kPTEKindSwap: {
			vm_mdl_t *mdl;
			iop_t *iop;
			struct vmp_pager_state *pgstate;
			vm_page_t *page;
			pfn_t drumslot = vmp_md_soft_pte_pfn(pte);
			int r;

			kassert(prototype == 0);

			if (!create) {
				memcpy(state->pgtable_pages, pages,
				    sizeof(pages));
				vmp_pte_wire_state_release(state, prototype);
				return -level;
			}

			/* newly-allocated page is retained */
			r = vmp_page_alloc_locked(&page,
			    prototype == 0 ? kPageUsePML1 + (level - 2) :
					     kPageUseVPML1 + (level - 2),
			    false);
			kassert(r == 0);

			pages[level - 2] = page;

			/* manually adjust the new page, includes our pinning */
			page->process = ps;
			page->nonzero_ptes++;
			page->noswap_ptes++;
			page->referent_pte = V2P(pte);
			page->drumslot = drumslot;

			pgstate = kmem_xalloc(sizeof(*pgstate), kVMemPFNDBHeld);
			kassert(r == 0);

			pgstate->refcnt = 1;
			pgstate->vpfn = (vaddr / PGSIZE);
			pgstate->length = 1;
			ke_event_init(&pgstate->event, false);

			page->pager_request = pgstate;
			page->dirty = false;

			/* create busy PTE */
			if (prototype == 0)
				vmp_md_busy_table_pointers(ps->vm,
				    pages[level - 1], pte, page);
			else {
				kfatal("implement\n");
			}

			vmstat.n_table_pageins++;

			vmp_release_pfn_lock(kIPLAST);
			ke_mutex_release(&ps->vm->mutex);

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

			KE_WAIT(&ps->vm->mutex, false, false, -1);
			vmp_acquire_pfn_lock();

			if (prototype == 0)
				vmp_md_setup_table_pointers(ps->vm,
				    pages[level - 1], page, pte, kWasTrans);
			else {
				vmp_md_pte_create_hw(pte, page->pfn, true,
				    false);
				vmp_pagetable_page_noswap_pte_created(ps->vm,
				    pages[level - 1], false);
			}

			setup_pte_counts(page);

			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, pte_level));

			break;
		}

		case kPTEKindZero: {
			vm_page_t *page;
			int r;

			if (!create) {
				memcpy(state->pgtable_pages, pages,
				    sizeof(pages));
				vmp_pte_wire_state_release(state, prototype);
				return -level;
			}

			/* newly-allocated page is retained */
			r = vmp_page_alloc_locked(&page,
			    prototype == 0 ? kPageUsePML1 + (level - 2) :
					     kPageUseVPML1 + (level - 2),
			    false);
			kassert(r == 0);

			pages[level - 2] = page;

			/* manually adjust the new page, includes our pinning */
			page->process = ps;
			page->nonzero_ptes++;
			page->noswap_ptes++;
			page->referent_pte = V2P(pte);

			/*
			 * this function also amends the directory's vm_page_t
			 * to add to its valid and nonzero PTE counts the
			 * needful
			 */
			if (prototype == 0)
				vmp_md_setup_table_pointers(ps->vm,
				    pages[level - 1], page, pte, kWasZero);
			else {
				vmp_md_pte_create_hw(pte, page->pfn, true,
				    false);
				vmp_pagetable_page_noswap_pte_created(ps->vm,
				    pages[level - 1], true);
			}

			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, pte_level));

#if 0
			if (prototype == 0) {
				vmp_release_pfn_lock(kIPLAST);
				r = vmp_wsl_insert(ps->vm,
				    vm_page_direct_map_addr(page), true);
				kassert(r == 0);
				vmp_acquire_pfn_lock();
			}
#endif

			break;
		}

		default:
			kfatal("Unexpected page table PTE state!\n");
		}
	}
	kfatal("unreached\n");
}

int
vmp_fetch_pte(vm_procstate_t *vmps, vaddr_t vaddr, pte_t **pte_out)
{
	int indexes[VMP_TABLE_LEVELS + 1];
	pte_t *table;

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	vmp_addr_unpack(vaddr, indexes);

	table = (pte_t*)P2V(vmps->md.table);

	for (int level = VMP_TABLE_LEVELS; level > 0; level--) {
		pte_t *pte = &table[indexes[level]];

		/* note - level is 1-based */

		if (level == 1) {
			*pte_out = pte;
			return 0;
		}

		if (vmp_pte_characterise(pte) != kPTEKindValid)
			return -1;

		table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
	}
	kfatal("unreached\n");
}

int
vmp_unmap_range(vm_procstate_t *vmps, vaddr_t start, vaddr_t end)
{
	pte_t *pte = NULL;
	struct vmp_pte_wire_state pte_wire;
	ipl_t ipl;
	int r;

	eprocess_t *ps = kernel_process;

	// KE_WAIT(&vmps->mutex, false, false, -1);
	ipl = vmp_acquire_pfn_lock();

	for (vaddr_t addr = start; addr < end; addr += PGSIZE) {
	skipped:

		if (pte == NULL || ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
			if (pte != NULL)
				vmp_pte_wire_state_release(&pte_wire, false);

			pte = NULL;

			r = vmp_wire_pte(kernel_process, addr, 0, &pte_wire,
			    false);
			if (r < 0) {
				uintptr_t align = PGSIZE;

				switch (-r) {
				case 4:
					align *= VMP_LEVEL_3_ENTRIES;
				case 3:
					align *= VMP_LEVEL_2_ENTRIES;
				case 2:
					align *= VMP_LEVEL_1_ENTRIES;
					break;

				default:
					kfatal("Unexpected offset %d\n", r);
				}

				addr = ROUNDUP(addr + 1, align);
				if (addr >= end)
					break;

				goto skipped;
			}

			pte = pte_wire.pte;
		}

		switch (vmp_pte_characterise(pte)) {
		case kPTEKindValid: {
			vm_page_t *page = vmp_pte_hw_page(pte, 1);
			vm_page_t *pte_page = vm_paddr_to_page(
			    PGROUNDDOWN(V2P(pte)));

			vmp_wsl_remove(vmps, addr, true);
			vmp_md_pte_create_zero(pte);
			vmp_pagetable_page_pte_deleted(vmps, pte_page, false);
			ki_tlb_flush_vaddr_globally(addr);

			if (page->use == kPageUseAnonPrivate)
				vmp_page_delete_locked(page);

			vmp_page_release_locked(page);

			break;
		}

		case kPTEKindTrans: {
			vm_page_t *page = vm_pfn_to_page(
			    vmp_md_soft_pte_pfn(pte));
			vm_page_t *pte_page = vm_paddr_to_page(
			    PGROUNDDOWN(V2P(pte)));

			vmp_md_pte_create_zero(pte);
			vmp_pagetable_page_pte_deleted(vmps, pte_page, false);

			if (page->use == kPageUseAnonPrivate)
				vmp_page_delete_locked(page);

			break;
		}

		case kPTEKindBusy:
			kfatal("Not handled yet\n");

		case kPTEKindSwap: {
			vm_page_t *pte_page = vm_paddr_to_page(
			    PGROUNDDOWN(V2P(pte)));
			vmp_md_pte_create_zero(pte);
			vmp_pagetable_page_pte_deleted(vmps, pte_page, true);
			/* TODO: free swap slot! */
			break;
		}

		case kPTEKindZero:
			continue;
		}
	}

	if (pte != NULL)
		vmp_pte_wire_state_release(&pte_wire, false);

	vmp_release_pfn_lock(ipl);

	return 0;
}
