/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file fork.c
 * @brief Virtual memory fork operation.
 */

#include <sys/k_log.h>
#include <sys/kmem.h>

#include <vm/map.h>
#include <vm/page.h>

struct fork_state {
	size_t npriv;
	size_t anonindex;
	struct vm_anon **anons;
};

kspinlock_t anon_creation_lock;
kspinlock_t anon_stealing_lock;

static struct vm_anon *
anon_retain(pte_t pte)
{
	struct vm_anon *forkpage = NULL;

	switch (pmap_pte_characterise(pte)) {
	case kPTEKindHW: {
		vm_page_t *page = pmap_pte_hwleaf_page(pte, 0);
		kassert(page->use == VM_PAGE_ANON_FORKED);
		forkpage = page->owner_anon;
		break;
	}

	case kPTEKindFork: {
		/* forkpage is directly in the PTE */
		forkpage = pmap_pte_soft_anon(pte);
		break;
	}

	default:
		kfatal("anon_retain: illegal pte kind %d\n",
		    pmap_pte_characterise(pte));
	}

	forkpage->refcount++;

	return forkpage;
}

static bool
is_private(pte_t pte)
{
	switch (pmap_pte_characterise(pte)) {
	case kPTEKindZero:
		/* no page, nothing to do. */
	case kPTEKindFork:
		/* handled elsewhere */
		return false;

	/* Trans and swap can only happen to private. */
	case kPTEKindTrans:
	case kPTEKindSwap:
		return true;

	case kPTEKindBusy:
		kfatal("Handle me\n");

	case kPTEKindHW: {
		vm_page_t *page = pmap_pte_hwleaf_page(pte, 0);
		return page->use == VM_PAGE_PRIVATE;
	}

	default:
		kfatal("illegal\n");
	}
}

static bool
is_fork(pte_t pte)
{
	switch (pmap_pte_characterise(pte)) {
	case kPTEKindZero:
		/* no page, nothing to do. */
	case kPTEKindTrans:
	case kPTEKindSwap:
		/* Trans and swap can only happen to private. */
		return false;

	case kPTEKindBusy:
		kfatal("is_fork: handle busy pte\n");

	case kPTEKindFork:
		return true;

	case kPTEKindHW: {
		vm_page_t *page = pmap_pte_hwleaf_page(pte, 0);
		return page->use == VM_PAGE_ANON_FORKED;

	default:
		kfatal("illegal\n");
	}
	}
}

static void
convert_page(vm_page_t *page, struct vm_anon *anon)
{
	vm_domain_t *dom = &vm_domains[page->domain];

	if (page->use != VM_PAGE_PRIVATE)
		kfatal("page %p: not anon private!\n", page);

	ke_spinlock_enter_nospl(&dom->queues_lock);
	dom->use_n[VM_PAGE_PRIVATE]--;
	dom->use_n[VM_PAGE_ANON_FORKED]++;
	page->use = VM_PAGE_ANON_FORKED;
	page->owner_anon = anon;
	page->pte = &anon->pte;
	page->shared.share_count = 1; /* only in original map is it mapped */

	pmap_pte_hwleaf_create(&anon->pte, VM_PAGE_PFN(page), PMAP_L0, 0, 0);
	ke_spinlock_exit_nospl(&dom->queues_lock);
}

static struct vm_anon *
convert_private_to_fork(struct fork_state *fork_state, vm_map_t *vmps,
    vaddr_t addr, pte_t *ppte)
{
	struct vm_anon *forkpage;
	pte_t pte;

	kassert(fork_state->anonindex < fork_state->npriv);
	forkpage = fork_state->anons[fork_state->anonindex++];

	ke_spinlock_enter_nospl(&anon_creation_lock);
	ke_spinlock_enter_nospl(&anon_stealing_lock);

	pte =  pmap_load_pte(ppte);

	switch (pmap_pte_characterise(pte)) {
	case kPTEKindHW: {
		vm_page_t *page = pmap_pte_hwleaf_page(pte, PMAP_L0);
		convert_page(page, forkpage);
		pmap_pte_hwleaf_clear_writeable(ppte);
		break;
	}

	case kPTEKindTrans: {
		vm_page_t *page = pmap_pte_hwleaf_page(pte, 0);
		convert_page(page, forkpage);
		pmap_pte_anon_create(ppte, forkpage, false);
		/* FIXME: fork replacing trans - we should drop noswap count? */
		kdprintf("VM: FIXME: fork replacing trans\n");
		break;
	}

	case kPTEKindSwap: {
		forkpage->pte = pte;
		pmap_pte_anon_create(ppte, forkpage, false);
		break;
	}

	default:
		kfatal("Illegal\n");
	}

	forkpage->refcount = 2;

	ke_spinlock_exit_nospl(&anon_stealing_lock);
	ke_spinlock_exit_nospl(&anon_creation_lock);

	return forkpage;
}

static int
cow_pages(struct fork_state *fork_state, vm_map_t *parent, vm_map_t *child,
    vaddr_t start, vaddr_t end)
{
	pte_t *ppte1 = NULL;
	struct pte_cursor pte_wire1, pte_wire2;
	struct vm_anon *forkpage;
	ipl_t ipl;
	int r;

	ipl = ke_spinlock_enter(&parent->rs.map->creation_lock);
	ke_spinlock_enter_nospl(&parent->rs.map->stealing_lock);

	for (vaddr_t addr = start; addr < end; addr += PGSIZE) {
		pte_t pte1;

	skipped:

		if (ppte1 == NULL || ((uintptr_t)(++ppte1) & (PGSIZE - 1)) == 0) {
			if (ppte1 != NULL)
				pmap_unwire_pte(parent, &parent->rs, &pte_wire1);

			ppte1 = NULL;

			r = pmap_wire_pte(parent, &parent->rs, &pte_wire1, addr, false);
			if (r < 0) {
				uintptr_t align = PGSIZE;

				switch (-r) {
#if PMAP_LEVELS >= 4
				case 3:
					align *= PMAP_L2_SKIP; /* fallthrough */
#endif
#if PMAP_LEVELS >= 3
				case 2:
					align *= PMAP_L1_SKIP; /* fallthrough */
#endif
				case 1:
					align *= PMAP_L0_SKIP;
					break;
				default:
					kfatal("Unexpected offset %d\n", -r);
				}

				kdprintf("fork: skipping by align 0x%zx\n",
				    align);

				addr = roundup2(addr + 1, align);
				if (addr >= end)
					break;

				goto skipped;
			}

			ppte1 = pte_wire1.pte;
		}

		pte1 = pmap_load_pte(ppte1);

		if (is_private(pte1)) {
			/* has to make PTE read-only, but can wait till end for
			 * shootdown ... */
			forkpage = convert_private_to_fork(fork_state, parent,
			    addr, ppte1);
		} else if (is_fork(pte1)) {
			ke_spinlock_enter_nospl(&anon_creation_lock);
			forkpage = anon_retain(pte1);
			ke_spinlock_exit_nospl(&anon_creation_lock);
		} else {
			continue;
		}

		ke_spinlock_exit_nospl(&parent->rs.map->stealing_lock);
		ke_spinlock_exit(&parent->rs.map->creation_lock, ipl);

		/* now that we have the forkpage, put it into the child pte */

		ipl = ke_spinlock_enter(&child->rs.map->creation_lock);
		ke_spinlock_enter_nospl(&child->rs.map->stealing_lock);

		r = pmap_wire_pte(child, &child->rs, &pte_wire2, addr, true);
		kassert(r == 0);

		pmap_pte_anon_create(pte_wire2.pte, forkpage, false);
		pmap_new_leaf_fork_ptes_created(&child->rs, &pte_wire2, 1);

		pmap_unwire_pte(child, &child->rs, &pte_wire2);

		ke_spinlock_exit_nospl(&child->rs.map->stealing_lock);
		ke_spinlock_exit(&child->rs.map->creation_lock, ipl);

		/* reacquire parent locks for next round */
		ipl = ke_spinlock_enter(&parent->rs.map->creation_lock);
		ke_spinlock_enter_nospl(&parent->rs.map->stealing_lock);
	}

	if (ppte1 != NULL)
		pmap_unwire_pte(parent, &parent->rs, &pte_wire1);

	ke_spinlock_exit_nospl(&parent->rs.map->stealing_lock);
	ke_spinlock_exit(&parent->rs.map->creation_lock, ipl);

	return 0;
}

int
vm_fork(vm_map_t *src_map, vm_map_t *dst_map)
{
	struct vm_map_entry *entry;
	struct fork_state forkstate;

	/*
	 * First, acquire exclusively the map locks..
	 * This inhibits the changing of its map and the creation of new
	 * private pages.
	 */
	ke_rwlock_enter_write(&src_map->map_lock, "fork parent");

	forkstate.npriv = src_map->rs.private_pages_n;
	forkstate.anonindex = 0;
	forkstate.anons = kmem_alloc(forkstate.npriv * sizeof(struct vm_anon *));
	for (size_t i = 0; i < forkstate.npriv; i++)
		forkstate.anons[i] = kmem_alloc(sizeof(struct vm_anon));

	RB_FOREACH(entry, vm_map_tree, &src_map->entries) {
		int r;
		vaddr_t vaddr;

		vaddr = entry->start;

		if (entry->is_phys) {
			r = vm_map_phys(dst_map, entry->phys_base, &vaddr,
			    entry->end - entry->start, entry->prot,
			    entry->cache, true);
			kassert(r == 0);
			continue;
		}

		r = vm_map(dst_map, entry->object, &vaddr,
		    entry->end - entry->start, entry->offset, entry->prot,
		    entry->max_prot, entry->inherit_shared, entry->cow, true);
		kassert(r == 0);

		if (entry->cow ||
		    (!entry->inherit_shared && entry->object == NULL)) {
			cow_pages(&forkstate, src_map, dst_map, vaddr,
			    entry->end);
		}
	}

	ke_rwlock_enter_write(&dst_map->map_lock, "fork child");

	kassert(forkstate.anonindex == forkstate.npriv);

	src_map->rs.private_pages_n = 0;

	pmap_tlb_flush_all_globally();

	ke_rwlock_exit_write(&dst_map->map_lock);
	ke_rwlock_exit_write(&src_map->map_lock);

	return 0;
}
