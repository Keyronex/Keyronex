/*!
 * @file fork.c
 * @brief Implements the "Fork Process Memory" operation.
 */

#include <kdk/executive.h>
#include <kdk/kmem.h>
#include <kdk/vm.h>

#include "kern/ki.h"
#include "vmp.h"

struct fork_state {
	size_t n_anonymous;
	size_t forkpage_index;
	struct vmp_forkpage **forkpages;
};

static bool
is_private(pte_t *pte)
{
	switch (vmp_pte_characterise(pte)) {
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

	case kPTEKindValid: {
		vm_page_t *page = vmp_pte_hw_page(pte, 1);
		return page->use == kPageUseAnonPrivate ||
		    page->use == kPageUseAnonFork;

	default:
		kfatal("illegal\n");
	}
	}
}

static bool
is_fork(pte_t *pte)
{
	switch (vmp_pte_characterise(pte)) {
	case kPTEKindZero:
		/* no page, nothing to do. */
	case kPTEKindTrans:
	case kPTEKindSwap:
		/* Trans and swap can only happen to private. */
		return false;

	case kPTEKindBusy:
		kfatal("Handle me\n");

	case kPTEKindFork:
		return true;

	case kPTEKindValid: {
		vm_page_t *page = vmp_pte_hw_page(pte, 1);
		return page->use == kPageUseAnonFork;

	default:
		kfatal("illegal\n");
	}
	}
}

static void
convert_page(pfn_t pfn, struct vmp_forkpage *forkpage)
{
	vm_page_t *page = vm_paddr_to_page(vmp_pfn_to_paddr(pfn));

	kassert(page->use == kPageUseAnonPrivate);
	vmstat.nanonprivate--;
	vmstat.nanonfork++;
	vmstat.nanonprivate--;
	vmstat.nanonfork++;
	page->use = kPageUseAnonFork;
	page->owner = forkpage;

	vmp_md_pte_create_hw(&forkpage->pte, pfn, true, true, true, true);
}

static struct vmp_forkpage *
convert_private_to_fork(struct fork_state *fork_state, vm_procstate_t *vmps,
    vaddr_t addr, pte_t *pte)
{
	struct vmp_forkpage *forkpage;

	kassert(fork_state->forkpage_index < fork_state->n_anonymous);
	forkpage = fork_state->forkpages[fork_state->forkpage_index++];

	switch (vmp_pte_characterise(pte)) {
	case kPTEKindValid: {
		pfn_t pfn = vmp_md_pte_hw_pfn(pte, 1);
		convert_page(pfn, forkpage);
		vmp_md_pte_hw_set_readonly(pte);
		break;
	}

	case kPTEKindTrans: {
		pfn_t pfn = vmp_md_soft_pte_pfn(pte);
		convert_page(pfn, forkpage);
		vmp_md_pte_create_fork(pte, forkpage);
		break;
	}

	case kPTEKindSwap: {
		forkpage->pte = *pte;
		vmp_md_pte_create_fork(pte, forkpage);
		break;
	}

	default:
		kfatal("Illegal\n");
	}

	forkpage->refcount = 2;

	return forkpage;
}

static struct vmp_forkpage *
forkpage_retain(pte_t *pte)
{
	struct vmp_forkpage *forkpage;

	forkpage = vmp_md_soft_pte_forkpage(pte);
	forkpage->refcount++;

	return forkpage;
}

static int
cow_pages(struct fork_state *fork_state, eprocess_t *parent, eprocess_t *child,
    vaddr_t start, vaddr_t end)
{
	vm_procstate_t *vmps1 = parent->vm, *vmps2 = child->vm;
	pte_t *pte1 = NULL;
	struct vmp_pte_wire_state pte_wire1, pte_wire2;
	struct vmp_forkpage *forkpage;
	ipl_t ipl;
	int r;

	KE_WAIT(&vmps1->ws_mutex, false, false, -1);
	ipl = vmp_acquire_pfn_lock();

	for (vaddr_t addr = start; addr < end; addr += PGSIZE) {
	skipped:

		if (pte1 == NULL || ((uintptr_t)(++pte1) & (PGSIZE - 1)) == 0) {
			if (pte1 != NULL)
				vmp_pte_wire_state_release(&pte_wire1, false);

			pte1 = NULL;

			r = vmp_wire_pte(parent, addr, 0, &pte_wire1, false);
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

			pte1 = pte_wire1.pte;
		}

		if (is_private(pte1)) {
			/* has to make PTE read-only, but can wait till end for
			 * shootdown ... */
			forkpage = convert_private_to_fork(fork_state, vmps1,
			    addr, pte1);
		} else if (is_fork(pte1))
			forkpage = forkpage_retain(pte1);
		else
			continue;

		vmp_release_pfn_lock(ipl);
		ke_mutex_release(&vmps1->ws_mutex);

		/* now that we have the forkpage, put it into the child pte */

		KE_WAIT(&vmps2->ws_mutex, false, false, -1);
		ipl = vmp_acquire_pfn_lock();

		r = vmp_wire_pte(child, addr, 0, &pte_wire2, true);
		kassert(r == 0);

		/* set up a standby fork pte */
		vmp_md_pte_create_fork(pte_wire2.pte, forkpage);
		vmp_pte_wire_state_release(&pte_wire2, false);

		vmp_release_pfn_lock(ipl);
		ke_mutex_release(&vmps2->ws_mutex);

		/* and reacquire parent working set lock & PFN lock */

		KE_WAIT(&vmps1->ws_mutex, false, false, -1);
		ipl = vmp_acquire_pfn_lock();
	}

	if (pte1 != NULL)
		vmp_pte_wire_state_release(&pte_wire1, false);

	vmp_release_pfn_lock(ipl);
	ke_mutex_release(&vmps1->ws_mutex);

	return 0;
}

int
vm_fork(eprocess_t *parent, eprocess_t *child)
{
	vm_map_entry_t *entry;
	struct fork_state fork_state;
	ipl_t ipl;

	/*
	 * First, acquire exclusively the map locks of parent.
	 * This inhibits the changing of its map and the creation of new
	 * private pages.
	 * There is no need to acquire the child's lock - there's nothing else
	 * can touch it yet.
	 */
	ex_rwlock_acquire_write(&parent->vm->map_lock, "fork parent");

	fork_state.n_anonymous = parent->vm->n_anonymous;
	fork_state.forkpage_index = 0;
	fork_state.forkpages = kmem_alloc(
	    fork_state.n_anonymous * sizeof(struct vmp_forkpage *));
	for (size_t i = 0; i < fork_state.n_anonymous; i++) {
		fork_state.forkpages[i] = kmem_alloc(
		    sizeof(struct vmp_forkpage));
	}

	RB_FOREACH (entry, vm_map_entry_rbtree, &parent->vm->vad_queue) {
		int r;
		vaddr_t vaddr;

		vaddr = entry->start;
		r = vm_ps_map_object_view(child->vm, entry->object, &vaddr,
		    entry->end - entry->start, entry->flags.offset * PGSIZE,
		    entry->flags.protection, entry->flags.max_protection,
		    entry->flags.inherit_shared, entry->flags.cow, true);
		kassert(r == 0);

		if (entry->flags.cow ||
		    (!entry->flags.inherit_shared && entry->object == NULL)) {
			ex_rwlock_acquire_write(&child->vm->map_lock,
			    "fork child");
			cow_pages(&fork_state, parent, child, vaddr,
			    entry->end);
			ex_rwlock_release_write(&child->vm->map_lock);
		}
	}

	kassert(fork_state.forkpage_index == fork_state.n_anonymous);

	parent->vm->n_anonymous = 0;

	ipl = vmp_acquire_pfn_lock();
	ki_tlb_flush_globally();
	vmp_release_pfn_lock(ipl);

	ex_rwlock_release_write(&parent->vm->map_lock);

	return 0;
}
