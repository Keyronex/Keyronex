#include "kdk/vm.h"
#include "kdk/kmem.h"
#include "vmp.h"
#include "nanokern/ki.h"

struct vmp_wsle {
	TAILQ_ENTRY(vmp_wsle) queue_entry;
	RB_ENTRY(vmp_wsle) rb_entry;
	vaddr_t vaddr;
};

static inline intptr_t
wsle_cmp(struct vmp_wsle *x, struct vmp_wsle *y)
{
	return x->vaddr - y->vaddr;
}

RB_GENERATE(vmp_wsle_tree, vmp_wsle, rb_entry, wsle_cmp);

static struct vmp_wsle *
vmp_wsl_find(vm_procstate_t *ps, vaddr_t vaddr)
{
	struct vmp_wsle key;
	key.vaddr = vaddr;
	return RB_FIND(vmp_wsle_tree, &ps->wsl.tree, &key);
}

void
vmp_page_evict(vm_procstate_t *vmps, pte_t *pte, vm_page_t *pte_page,
    vaddr_t vaddr)
{
	bool dirty = vmp_md_hw_pte_is_writeable(pte);
	vm_page_t *page = vmp_pte_hw_page(pte, 1);

	page->dirty |= dirty;

	switch (page->use) {
	case kPageUseAnonPrivate: {
		/*
		 * we need to replace this with a transition PTE then.
		 * used_ptes and noswap_ptes count is as such unchanged.
		 */
		vmp_md_pte_create_trans(pte, page->pfn);
		ki_tlb_flush_vaddr_globally(vaddr);
		vmp_page_release_locked(page);
		break;
	}

	case kPageUseFileShared:
		vmp_md_pte_create_zero(pte);
		vmp_pagetable_page_pte_deleted(vmps, pte_page, false);
		ki_tlb_flush_vaddr_globally(vaddr);
		vmp_page_release_locked(page);
		break;

	default:
		kfatal("Unhandled page use in working set eviction\n");
	}
}

void
wsl_evict_one(vm_procstate_t *vmps)
{
	struct vmp_wsle *wsle = TAILQ_FIRST(&vmps->wsl.queue);
	vm_page_t *pte_page;
	pte_t *pte;
	int r;
	ipl_t ipl;

	kassert(wsle != NULL);
	TAILQ_REMOVE(&vmps->wsl.queue, wsle, queue_entry);
	RB_REMOVE(vmp_wsle_tree, &vmps->wsl.tree, wsle);

	ipl = vmp_acquire_pfn_lock();
	r = vmp_fetch_pte(vmps, wsle->vaddr, &pte);
	kassert(r == 0);
	kassert(vmp_md_pte_is_valid(pte));

#ifdef TRACE_WS
	kprintf("Evicting 0x%zx\n", wsle->vaddr);
#endif
	pte_page = vm_paddr_to_page(PGROUNDDOWN(V2P(pte)));

	vmp_page_evict(vmps, pte, pte_page, wsle->vaddr);
	vmp_release_pfn_lock(ipl);

	kmem_free(wsle, sizeof(*wsle));
	vmps->wsl.ws_current_count--;
}

int
vmp_wsl_insert(vm_procstate_t *ps, vaddr_t vaddr, bool locked)
{
	struct vmp_wsle *wsle;

	kassert(vmp_wsl_find(ps, vaddr) == NULL);

	if (ps->wsl.ws_current_count >= ps->wsl.max) {
		wsl_evict_one(ps);
	}

	if (locked)
		ps->wsl.locked_count++;
	ps->wsl.ws_current_count++;

	wsle = kmem_alloc(sizeof(*wsle));
	wsle->vaddr = vaddr;
	if (!locked)
		TAILQ_INSERT_TAIL(&ps->wsl.queue, wsle, queue_entry);
	RB_INSERT(vmp_wsle_tree, &ps->wsl.tree, wsle);

	return 0;
}

void
vmp_wsl_remove(vm_procstate_t *ps, vaddr_t vaddr, bool pfn_locked)
{
	struct vmp_wsle *wsle = vmp_wsl_find(ps, vaddr);
	kassert(wsle != NULL);
	RB_REMOVE(vmp_wsle_tree, &ps->wsl.tree, wsle);
	TAILQ_REMOVE(&ps->wsl.queue, wsle, queue_entry);
	kmem_xfree(wsle, sizeof(*wsle), pfn_locked ? kVMemPFNDBHeld : 0);
	ps->wsl.ws_current_count--;
}

void
vmp_wsl_dump(vm_procstate_t *ps)
{
	struct vmp_wsle *wsle;
	kprintf("WSL: %zu entries, %zu locked enties, %zu max\n",
	    ps->wsl.ws_current_count, ps->wsl.locked_count, ps->wsl.max);
	kprintf("All entries:\n");
	RB_FOREACH (wsle, vmp_wsle_tree, &ps->wsl.tree) {
		kprintf("- 0x%zx\n", (size_t)wsle->vaddr);
	}
	kprintf("Dynamic Entries:\n");
	TAILQ_FOREACH (wsle, &ps->wsl.queue, queue_entry) {
		kprintf("- 0x%zx\n", (size_t)wsle->vaddr);
	}
}
