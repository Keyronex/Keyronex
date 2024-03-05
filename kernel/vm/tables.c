#include "kdk/nanokern.h"
#include "vmp.h"

static bool
page_is_root_table(vm_page_t *page)
{
	return page->use == (kPageUsePML1 + (VMP_TABLE_LEVELS - 1));
}

void
vmp_pagetable_page_valid_pte_created(kprocess_t *ps, vm_page_t *page,
    bool is_new)
{
	vmp_page_retain_locked(page);
	if (is_new)
		page->nonzero_ptes++;
	if (page->valid_ptes++ == 0 && !page_is_root_table(page)) {
#if PAGETABLE_PAGING
		vmp_wsl_lock_entry(ps, P2V(vmp_page_paddr(page)));
#else
		(void)0;
#endif
	}
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
static void
vmp_pagetable_page_pte_deleted(kprocess_t *ps, vm_page_t *page, bool was_swap)
{
	if (page->nonzero_ptes-- == 1 && !page_is_root_table(page)) {
		vm_page_t *dirpage;

		page->use = kPageUseDeleted;

#if PAGETABLE_PAGING
		if (page->nonswap_ptes == 1) {
			vmp_wsl_unlock_entry(ps, P2V(vmp_page_paddr(page)));
			vmp_wsl_remove(ps, P2V(vmp_page_paddr(page)));
		} else if (page->nonswap_ptes == 0)
			vmp_wsl_remove(ps, P2V(vmp_page_paddr(page)));
		else
			kfatal("expectex nonswap_ptes to be 0 or 1\n");
#endif

		dirpage = vm_paddr_to_page(page->referent_pte);
#if 0
		vmp_md_delete_table_pointers(ps, dirpage,
		    (pte_t *)P2V(page->referent_pte));
#endif

		page->valid_ptes = 0;
		page->referent_pte = 0;

#if PAGETABLE_PAGING
		/*! once for the working set removal.... */
		vmp_page_release_locked(page);
#endif
		/*! and once for the PTE zeroing; this will free the page. */
		vmp_page_release_locked(page);

		return;
	}
	if (!was_swap && page->valid_ptes-- == 1 && !page_is_root_table(page)) {
#if PAGETABLE_PAGING
		vmp_wsl_unlock_entry(ps, P2V(vmp_page_paddr(page)));
#else
		(void)0;
#endif
	}
	vmp_page_release_locked(page);
}

void
vmp_pte_wire_state_release(struct vmp_pte_wire_state *state)
{
	for (int i = 0; i < VMP_TABLE_LEVELS; i++) {
		if (state->pgtable_pages[i] == NULL)
			continue;
		vmp_pagetable_page_pte_deleted(state->pgtable_pages[i]->process,
		    state->pgtable_pages[i], false);
	}
}

/*!
 * Note: WS lock and PFN lock will be locked and unlocked regularly here.
 * \pre VAD list mutex held
 * \pre WS mutex held
 * \pre PFN database lock held
 */
int
vmp_wire_pte(kprocess_t *ps, vaddr_t vaddr, struct vmp_pte_wire_state *state)
{
	ipl_t ipl;
	int indexes[VMP_TABLE_LEVELS + 1];
	vm_page_t *pages[VMP_TABLE_LEVELS] = { 0 };
	pte_t *table;

	vmp_addr_unpack(vaddr, indexes);

	ipl = vmp_acquire_pfn_lock();

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
	table = (pte_t*)P2V(ps->vm->md.table);;
	pages[VMP_TABLE_LEVELS - 1] = vm_paddr_to_page(ps->vm->md.table);
	vmp_pagetable_page_valid_pte_created(ps, pages[VMP_TABLE_LEVELS - 1],
	    true);

	for (int level = VMP_TABLE_LEVELS; level > 0; level--) {
		pte_t *pte = &table[indexes[level]];

		/* note - level is 1-based */

		if (level == 1) {
			memcpy(state->pgtable_pages, pages, sizeof(pages));
			state->pte = pte;
			vmp_release_pfn_lock(ipl);
			return 0;
		}

#if DEBUG_TABLES
		printf("Dealing with pte in level %d; pte is %p; page is %p\n",
		    level, pte, pages[level - 1]);
#endif

	restart_level:
		switch (vmp_pte_characterise(pte)) {
		case kPTEKindValid: {
			vm_page_t *page = vmp_pte_hw_page(pte, level);
			pages[level - 2] = page;
			/* pin this next level */
			vmp_pagetable_page_valid_pte_created(ps, page, true);
			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
			break;
		}

		case kPTEKindZero: {
			vm_page_t *page;
			int r;

			/* newly-allocated page is retained */
			r = vmp_page_alloc_locked(&page,
			    kPageUsePML1 + (level - 2), false);
			kassert(r == 0);

			pages[level - 2] = page;

			/* manually adjust the new page, includes our pinning */
			vmp_page_retain_locked(page);
			page->process = ps;
			page->nonzero_ptes++;
			page->valid_ptes++;
			page->referent_pte = V2P(pte);

#if PAGETABLE_PAGING
			vmp_wsl_insert(ps, P2V(vmp_page_paddr(page)), true,
			    true);
#endif

			vmp_md_setup_table_pointers(ps, pages[level - 1], page,
			    pte, true);

			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
			break;
		}

#if PAGETABLE_PAGING
		case kPTEKindTrans: {
			paddr_t next_table_p = vmp_pfn_to_paddr(pte->trans.pfn);
			vm_page_t *page = vm_paddr_to_page(next_table_p);
			/* retain for our wiring purposes */
			pages[level - 2] = vmp_page_retain_locked(page);

			/* manually adjust the page */
			vmp_page_retain_locked(page);
			page->nonzero_ptes++;
			page->valid_ptes++;
			vmp_wsl_insert(ps, P2V(next_table_p), true, true);

			vmp_md_setup_table_pointers(ps, pages[level - 1], page,
			    pte, false);

			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, level));
			break;
		}

		case kPTEKindBusy: {
			vm_page_t *page = vmp_pte_hw_page(pte, level);
			vmp_pager_state_t *state = page->pager_state;
			state->refcount++;
			vmp_release_pfn_lock(ipl);
			ke_mutex_release(&ps->ws_lock);
			ke_event_wait(&state->event, -1);
			vmp_pager_state_release(state);
			ke_wait(&ps->ws_lock, "vmp_wire_pte: reacquire ws_lock",
			    false, false, -1);
			ipl = vmp_acquire_pfn_lock();
			goto restart_level;
		}

		case kPTEKindSwap:
			kfatal("swap page back in\n");
			break;
#endif

		default:
			kfatal("Unexpected page table PTE state!\n");
		}
	}
	kfatal("unreached\n");
}

int
vmp_fetch_pte(kprocess_t *ps, vaddr_t vaddr, bool create, pte_t **pte_out)
{
	int indexes[VMP_TABLE_LEVELS + 1];
	pte_t *table;

	vmp_addr_unpack(vaddr, indexes);

	table = (pte_t*)P2V(ps->vm->md.table);

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
