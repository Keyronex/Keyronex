#include "kdk/executive.h"
#include "kdk/libkern.h"
#include "kdk/nanokern.h"
#include "kdk/vm.h"
#include "vmp.h"

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
	if (page->noswap_ptes == 1 && !page_is_root_table(page)) {
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
void
vmp_pagetable_page_pte_deleted(vm_procstate_t *ps, vm_page_t *page,
    bool was_swap)
{
	kassert(ke_spinlock_held(&vmp_pfn_lock));

	if (page->nonzero_ptes-- == 1 && !page_is_root_table(page)) {
		vm_page_t *dirpage;
		enum vm_page_use use = page->use;

		vmp_page_delete_locked(page);

#if PAGETABLE_PAGING
		if (page->nonswap_ptes == 1) {
			vmp_wsl_unlock_entry(ps, P2V(vmp_page_paddr(page)));
			vmp_wsl_remove(ps, P2V(vmp_page_paddr(page)));
		} else if (page->nonswap_ptes == 0)
			vmp_wsl_remove(ps, P2V(vmp_page_paddr(page)));
		else
			kfatal("expectex nonswap_ptes to be 0 or 1\n");
#endif

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

#if PAGETABLE_PAGING
		/*! once for the working set removal.... */
		vmp_page_release_locked(page);
#endif
		/*! and once for the PTE zeroing; this will free the page. */
		vmp_page_release_locked(page);

		return;
	}
	if (!was_swap && page->noswap_ptes-- == 1 && !page_is_root_table(page)) {
#if PAGETABLE_PAGING
		vmp_wsl_unlock_entry(ps, P2V(vmp_page_paddr(page)));
#else
		(void)0;
#endif
	}
	vmp_page_release_locked(page);
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

/*!
 * \pre VAD list mutex held
 * \pre PFN database lock held
 */
int
vmp_wire_pte(eprocess_t *ps, vaddr_t vaddr, paddr_t prototype,
    struct vmp_pte_wire_state *state)
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

		case kPTEKindZero: {
			vm_page_t *page;
			int r;

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
				    pages[level - 1], page, pte, true);
			else {
				vmp_md_pte_create_hw(pte, page->pfn, true,
				    false);
				vmp_pagetable_page_noswap_pte_created(ps->vm,
				    pages[level - 1], true);
			}

			table = (pte_t *)P2V(vmp_pte_hw_paddr(pte, pte_level));
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
