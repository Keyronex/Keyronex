/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vm/rs.c
 * @brief Resident Set management.
 */

#include <sys/k_log.h>

#include <vm/map.h>

void
rs_evict_leaf_pte(vm_rs_t *rs, vaddr_t vaddr, vm_page_t *page, pte_t *ppte)
{
	pte_t pte = pmap_load_pte(ppte);
	bool dirty = pmap_pte_hwleaf_writeable(pte);

	switch (page->use) {
#if 0
	case VM_PAGE_PRIVATE: {
		pmap_pte_create_soft(pte, kPTEKindTrans, VM_PAGE_PFN(page),
		    true);
		pmap_tlb_flush_vaddr_globally(vaddr);
		// dcache flush?
		vm_page_release_and_dirty(page, dirty);
		break;
	}

	case VM_PAGE_ANON_SHARED:
	case VM_PAGE_FILE: {
		pmap_pte_hw_create_zero(pte);
		pmap_tlb_flush_vaddr_globally(vaddr);
		// dcache flush?
		/* factor: share count drop */

		spinlock_lock_nospl(&page->owner_obj->stealing_lock);
		page->shared.dirty |= dirty;
		if (--page->shared.share_count == 0)
			vm_page_release_and_dirty(page, page->shared.dirty);
		spinlock_unlock_nospl(&page->owner_obj->stealing_lock);

		break;
	}
#endif

	default: {
		kfatal("Implement rs_evict_leaf_pte for page use %d\n",
		    page->use);
	}
	}
}
