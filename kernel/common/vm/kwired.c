/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-2026 Cloudarox Solutions.
 */
/*
 * @file kwired.c
 * @brief Kernel wired memory allocator.
 */

#include <keyronex/dlog.h>
#include <keyronex/intr.h>
#include <keyronex/pmap.h>
#include <keyronex/proc.h>
#include <keyronex/vm.h>
#include <keyronex/vmem.h>
#include <keyronex/vmem_impl.h>

#include <libkern/lib.h>

#include "vm/map.h"

struct vm_rs vm_kwired_rs;
kspinlock_t kwired_lock = KSPINLOCK_INITIALISER;
vmem_t kwired_arena;

void
vm_kwired_init(void)
{
	vmem_init(&kwired_arena, "kernel-wired-heap", PIN_HEAP_BASE,
	    PIN_HEAP_SIZE, PGSIZE, NULL, NULL, NULL, 0, 0);
}

void *
vm_kwired_alloc(size_t npages, vm_alloc_flags_t flags)
{
	vmem_addr_t addr;
	ipl_t ipl;
	struct pte_cursor state;
	pte_t *pte = NULL;
	int r;

	if (npages == 1) {
		vm_page_t *page = vm_page_alloc(VM_PAGE_KWIRED, 0,
		    VM_DOMID_LOCAL, flags);
		if (page == NULL && (flags & VM_NOFAIL))
			kfatal("vm_kwired_alloc: out of memory on nofail");
		else if (page == NULL && (flags & VM_SLEEP))
			kfatal("vm_kwired_alloc: out of memory (sleep!)");
		else if (page == NULL)
			return NULL;
		memset((void *)vm_page_hhdm_addr(page), 0, PGSIZE);
		return (void *)vm_page_hhdm_addr(page);
	}

	ipl = ke_spinlock_enter(&kwired_lock);
	r = vmem_xalloc(&kwired_arena, npages << PGSHIFT, 0, 0, 0, 0, 0, flags,
	    &addr);
	if (r != 0) {
		/* vmem_xalloc should deal with this for us */
		kassert_dbg(!(flags & VM_NOFAIL), "");
		ke_spinlock_exit(&kwired_lock, ipl);
		return NULL;
	}

	ke_spinlock_enter_nospl(&proc0.vm_map->creation_lock);
	ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);

	for (size_t i = 0; i < npages; i++) {
		vm_page_t *page;

		if (i == 0 || ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
			if (pte != NULL)
				pmap_unwire_pte(proc0.vm_map, &vm_kwired_rs,
				    &state);

			r = pmap_wire_pte(proc0.vm_map, &vm_kwired_rs, &state,
			    addr + (i << PGSHIFT), true);
			kassert(r == 0);

			pte = state.pte;
		}

		ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
		page = vm_page_alloc(VM_PAGE_KWIRED, 0, VM_DOMID_LOCAL, flags);
		kassert(page != NULL);
		ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);

		memset((void *)vm_page_hhdm_addr(page), 0, PGSIZE);

		pmap_pte_hwleaf_create(pte, VM_PAGE_PFN(page), PMAP_L1,
		    VM_READ | VM_WRITE, kCacheModeDefault);
		state.pages[0]->proctable.nonzero_ptes++;
		state.pages[0]->proctable.noswap_ptes++;
	}
	pmap_unwire_pte(proc0.vm_map, &vm_kwired_rs, &state);

	ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
	ke_spinlock_exit_nospl(&proc0.vm_map->creation_lock);
	ke_spinlock_exit(&kwired_lock, ipl);

	return (void *)addr;
}

void
vm_kwired_free(void *ptr, size_t npages)
{
#if 0
	vmem_addr_t addr = (vmem_addr_t)ptr;
	ipl_t ipl;
	struct pte_cursor state;
	pte_t *pte = NULL;
	int r;

	if (npages == 1) {
		kassert((vaddr_t) ptr >= HHDM_BASE &&  (vaddr_t) ptr < HHDM_END, "");
		vm_page_delete(VM_PAGE_FOR_PADDR(v2p((vaddr_t)ptr)), true);
		return;
	}

	ipl = ke_spinlock_enter(&kwired_lock);
	ke_spinlock_enter_nospl(&proc0.vm_map->creation_lock);
	ke_spinlock_enter_nospl(&proc0.vm_map->stealing_lock);

	/* TODO: FREE THE VMEM AREA! Maybe do Mach style deferred release? */

	for (size_t i = 0; i < npages; i++) {
		vm_page_t *page;

		if (i == 0 || ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
			if (pte != NULL)
				pmap_unwire_pte(proc0.vm_map, &vm_kwired_rs, &state);

			r = pmap_wire_pte(proc0.vm_map, &vm_kwired_rs, &state,
			    addr + (i << PGSHIFT), true);
			kassert(r == 0);

			pte = state.pte;
		}

		page = pmap_pte_hw_page(pte, 0);

		/* TODO: zero the PTE.... */
		state.pages[0]->proctable.noswap_ptes--;
		state.pages[0]->proctable.nonzero_ptes--;

		vm_page_delete(page, true);
	}
	pmap_unwire_pte(proc0.vm_map, &vm_kwired_rs, &state);

	ke_spinlock_exit_nospl(&proc0.vm_map->stealing_lock);
	ke_spinlock_exit_nospl(&proc0.vm_map->creation_lock);
	ke_spinlock_exit(&kwired_lock, ipl);
#else
	/* do nothing for now... */
#endif
}
