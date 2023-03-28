/*
 * Copyright (c) 2022-2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */
/*!
 * @file vm/amd64/vmamd64.c
 * @brief Physical page mapping functionality.
 *
 * Virtually every one of these functions should be called with PFN database
 * lock held.
 */

#include <stdatomic.h>

#include "bsdqueue/queue.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/machdep.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "machdep/amd64/amd64.h"
#include "vm/vm_internal.h"

enum {
	kPML4Shift = 0x39,
	kPDPTShift = 0x30,
	kPDIShift = 0x21,
	kPTShift = 0x12,
};

enum {
	kMMUPresent = 0x1,
	kMMUWrite = 0x2,
	kMMUUser = 0x4,
	kMMUWriteThrough = 0x8,
	kMMUCacheDisable = 0x10,
	kMMUAccessed = 0x20, /* bit 5*/
	kMMUDirty = 0x40,    /* bit 6 */
	kPageGlobal = 0x100,

	kMMUDefaultProt = kMMUPresent | kMMUWrite | kMMUUser,

	kMMUFrame = 0x000FFFFFFFFFF000
};

struct pv_entry {
	LIST_ENTRY(pv_entry) list_entry;
	vm_map_t *map;
	vaddr_t vaddr;
};

typedef uint64_t pml4e_t, pdpte_t, pde_t, pte_t;

static uint64_t *pte_get_addr(uint64_t pte);
static void pte_set(uint64_t *pte, paddr_t addr, uint64_t flags);
void pmap_invlpg(vaddr_t addr);

void
pmap_kernel_init(void)
{
	uint64_t *cr3 = (void *)read_cr3();
	kernel_process.map->md.cr3 = (paddr_t)cr3;

	/* pre-allocate the top 256. they are globally shared. */
	for (int i = 255; i < 511; i++) {
		uint64_t *pml4 = P2V(cr3);
		if (pte_get_addr(pml4[i]) == NULL) {
			vm_page_t *page;
			vmp_page_alloc(kernel_process.map, true, kPageUseVMM,
			    &page);
			pte_set(&pml4[i], VM_PAGE_PADDR(page), kMMUDefaultProt);
		}
	}
}

void
pmap_new(struct vm_map *map)
{
	vm_page_t *page;

	ke_spinlock_init(&map->md.lock);
	vmp_page_alloc(map, true, kPageUseVMM, &page);
	map->md.cr3 = VM_PAGE_PADDR(page);

	/* copy over the higher half mappings */
	for (int i = 255; i < 512; i++) {
		uint64_t *pml4 = P2V(map->md.cr3);
		uint64_t *kpml4 = P2V(kernel_process.map->md.cr3);
		pte_set(&pml4[i], (paddr_t)kpml4[i], kMMUDefaultProt);
	}
}

/*! Free a level of page tables. \p table is a physical address. */
void
pmap_free_sub(vm_map_t *map, uint64_t *table, int level)
{
	vm_page_t *page;

	if (table == NULL)
		return;

	table = P2V(table);

	/*
	 * we don't free the individual mappings (there shouldn't *be*
	 * any left, as they should've been removed by vm_deallocate).
	 * Only the page tables themselves are freed.
	 */
	if (level > 1)
		for (int i = 0; i < 512; i++) {
			pte_t *entry = &table[i];
			pmap_free_sub(map, pte_get_addr(*entry), level - 1);
		}

	page = vmp_paddr_to_page((uintptr_t)V2P(table));
	vmp_page_free(map, page);
}

void
pmap_free(vm_map_t *map)
{
	uint64_t *vpml4 = P2V(map->md.cr3);
	for (int i = 0; i < 255; i++) {
		pte_t *entry = &vpml4[i];
		pmap_free_sub(map, pte_get_addr(*entry), 3);
	}
}

/* get the flags of a pte */
uint64_t *
pte_get_flags(uint64_t pte)
{
	return (uint64_t *)(pte & ~kMMUFrame);
}

/* get the physical address to which a pte points */
static uint64_t *
pte_get_addr(uint64_t pte)
{
	return (uint64_t *)(pte & kMMUFrame);
}

/* reset a pte to a given addy and flags. pte must be a virt addr. */
static void
pte_set(uint64_t *pte, paddr_t addr, uint64_t flags)
{
	uintptr_t a = (uintptr_t)addr;
	a &= kMMUFrame;
	*pte = 0x0;
	*pte = a | flags;
}

static uint64_t
vm_prot_to_i386(vm_protection_t prot)
{
	kassert(prot & kVMRead);
	return (prot & kVMRead ? kMMUPresent : 0) |
	    (prot & kVMWrite ? kMMUWrite : 0) | kMMUUser;
}

/*!
 * Get the physical address at which a page table entry points. optionally
 * allocate a new entry, setting appropriate flags. Table should be a pointer
 * to the physical location of the table.
 */
static uint64_t *
pmap_descend(vm_map_t *map, uint64_t *table, size_t index, bool alloc,
    uint64_t mmuprot)
{
	uint64_t *entry = P2V((&table[index]));
	uint64_t *addr = NULL;

	if (*entry & kMMUPresent) {
		addr = pte_get_addr(*entry);
	} else if (alloc) {
		vm_page_t *page;
		vmp_page_alloc(map, true, kPageUseVMM, &page);
		addr = (uint64_t *)VM_PAGE_PADDR(page);
		pte_set(entry, (paddr_t)addr, mmuprot);
	}

	return addr;
}

/*!
 * @returns (physical) pointer to the pte for this virtual address, or NULL if
 * none exists
 */
static pte_t *
pmap_fully_descend(vm_map_t *map, vaddr_t virt)
{
	uintptr_t virta = (uintptr_t)virt;
	pml4e_t *pml4 = (void *)map->md.cr3;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	pdpte_t *pdptes;
	pde_t *pdes;
	pte_t *ptes;

	pdptes = pmap_descend(map, pml4, pml4i, false, 0);
	if (!pdptes) {
		return 0x0;
	}

	pdes = pmap_descend(map, pdptes, pdpti, false, 0);
	if (!pdes) {
		return 0x0;
	}

	ptes = pmap_descend(map, pdes, pdi, false, 0);
	if (!ptes) {
		return 0x0;
	}

	return &ptes[pti];
}

paddr_t
pmap_trans(vm_map_t *map, vaddr_t virt)
{
	uintptr_t virta = (uintptr_t)virt;
	pml4e_t *pml4 = (void *)map->md.cr3;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	int pi = ((virta)&0xFFF);
	paddr_t r;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&map->md.lock);

	pdpte_t *pdpte;
	pde_t *pde;
	pte_t *pte;

	pdpte = pmap_descend(map, pml4, pml4i, false, 0);
	if (!pdpte) {
		return 0x0;
	}

	pde = pmap_descend(map, pdpte, pdpti, false, 0);
	if (!pde) {
		return 0x0;
	}

	pte = pmap_descend(map, pde, pdi, false, 0);
	if (!pte) {
		return 0x0;
	}

	pte = P2V(pte);

	if (!(pte[pti] & kMMUPresent))
		r = 0x0;
	else
		r = (paddr_t)pte_get_addr(pte[pti]) + pi;

	ke_spinlock_release(&map->md.lock, ipl);
	return r;
}

void
pmap_enter_common(vm_map_t *map, paddr_t phys, vaddr_t virt,
    vm_protection_t prot)
{
	uintptr_t virta = (uintptr_t)virt;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	pml4e_t *pml4 = (void *)map->md.cr3;
	pdpte_t *pdpte;
	pde_t *pde;
	pte_t *pt;
	pte_t *pti_virt;

	pdpte = pmap_descend(map, pml4, pml4i, true, kMMUDefaultProt);
	pde = pmap_descend(map, pdpte, pdpti, true, kMMUDefaultProt);
	pt = pmap_descend(map, pde, pdi, true, kMMUDefaultProt);

	pti_virt = P2V(&pt[pti]);
	void *oldaddr;

	uint64_t tib = 1024ul * 1024 * 1024 * 1024;
	kassert(phys < tib);

	if ((oldaddr = pte_get_addr(*pti_virt)) != NULL) {
		/*! this may turn out to be excessive */
		vmem_dump(&kernel_process.map->vmem);
		kmem_dump();
		kfatal("not remapping a PTE without explicit request\n"
		       "(requested vaddr=>phys 0x%lx=>0x%lx\n"
		       "(existing 0x%lx=>%p; PTE is %p)\n",
		    virt, phys, virt, oldaddr, &pt[pti]);
	}

	pte_set(pti_virt, phys,
	    vm_prot_to_i386(prot) | (virt < KAREA_BASE ? kMMUUser : 0));
}

void
pmap_enter(vm_map_t *map, paddr_t phys, vaddr_t virt, vm_protection_t prot)
{
	ipl_t ipl = ke_spinlock_acquire(&map->md.lock);
	pmap_enter_common(map, phys, virt, prot);
	ke_spinlock_release(&map->md.lock, ipl);
}

void
pmap_enter_pageable(vm_map_t *map, vm_page_t *page, vaddr_t virt,
    vm_protection_t prot)
{
	ipl_t ipl;
	struct pv_entry *pve;

	kassert((virt & 4095) == 0);

	pve = kmem_alloc(sizeof(*pve));
	pve->map = map;
	pve->vaddr = virt;

#if 0
	kdprintf("ENTER PVE %p at VADDR 0x%lx\n", pve, pve->vaddr);
#endif

	ipl = ke_spinlock_acquire(&map->md.lock);
	pmap_enter_common(map, VM_PAGE_PADDR(page), virt, prot);
	LIST_INSERT_HEAD(&page->pv_list, pve, list_entry);
	ke_spinlock_release(&map->md.lock, ipl);
}

vm_page_t *
pmap_unenter(vm_map_t *map, vaddr_t vaddr)
{
	paddr_t paddr;
	pte_t *pte;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&map->md.lock);

	pte = pmap_fully_descend(map, vaddr);

	kassert(pte);
	pte = P2V(pte);
	paddr = (paddr_t)pte_get_addr(*pte);

	kassert(*pte != 0x0);
	*pte = 0x0;

	ke_spinlock_release(&map->md.lock, ipl);

	return vmp_paddr_to_page(paddr);
}

int
pmap_unenter_pageable(vm_map_t *map, krx_out vm_page_t **out, vaddr_t virt)
{
	paddr_t paddr;
	pte_t *pte;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&map->md.lock);

	pte = pmap_fully_descend(map, virt);

	if (pte)
		pte = P2V(pte);

	if (pte && *pte != 0x0) {
		vm_page_t *page;

		paddr = (paddr_t)pte_get_addr(*pte);
		*pte = 0x0;

		page = vmp_paddr_to_page(paddr);
		if (page) {
			struct pv_entry *pve;
			bool found;

			LIST_FOREACH (pve, &page->pv_list, list_entry) {
				if (pve->map == map && pve->vaddr == virt) {
					found = true;
					LIST_REMOVE(pve, list_entry);
					kmem_free(pve, sizeof(*pve));
				}
			}

			/* should always be a PV entry for pageable mappings */
			kassert(found);
		}

		if (out)
			*out = page;
	}

	pmap_invlpg(virt);

	ke_spinlock_release(&map->md.lock, ipl);

	return 0;
}

void
pmap_unenter_pageable_range(vm_map_t *map, vaddr_t vaddr, vaddr_t end)
{
	for (; vaddr != end; vaddr += PGSIZE) {
		pmap_unenter_pageable(map, NULL, vaddr);
	}
}
void
pmap_protect_range(vm_map_t *map, vaddr_t base, vaddr_t end,
    vm_protection_t limit)
{
	kassert(limit == (kVMRead | kVMExecute));

	ipl_t ipl;

	ipl = ke_spinlock_acquire(&map->md.lock);

	/* this is really non-optimal and should be written properly */
	for (vaddr_t vaddr = base; vaddr < end; vaddr++) {
		pte_t *pte = pmap_fully_descend(map, vaddr);

		if (!pte)
			continue;

		pte = P2V(pte);
		if (pte_get_addr(*pte) == NULL)
			continue;

		if (*pte & kMMUWrite) {
			/* todo: tlb shootdown! */
			*pte &= ~kMMUWrite;
			pmap_invlpg(base);
		}
	}

	ke_spinlock_release(&map->md.lock, ipl);
}

#if 0
bool
pmap_is_present(vm_map_t *map, vaddr_t vaddr, paddr_t *paddr)
{
	pte_t *pte = pmap_fully_descend(map, vaddr);

	if (!pte)
		return false;

	pte = P2V(pte);

	if (!(*pte & kMMUPresent))
		return false;
	else {
		if (paddr)
			*paddr = (paddr_t)pte_get_addr(*pte);
		return true;
	}
}

bool
pmap_is_writeable(vm_map_t *map, vaddr_t vaddr, paddr_t *paddr)
{
	pte_t *pte = pmap_fully_descend(map, vaddr);

	if (!pte)
		return false;

	pte = P2V(pte);

	if (!(*pte & kMMUWrite))
		return false;
	else {
		if (paddr)
			*paddr = (paddr_t)pte_get_addr(*pte);
		return true;
	}
}
#endif

void
vm_map_md_init(vm_map_t *map)
{
	vm_page_t *page;
	vmp_page_alloc(map, true, kPageUseVMM, &page);
	map->md.cr3 = VM_PAGE_PADDR(page);
	for (int i = 255; i < 512; i++) {
		uint64_t *pml4 = P2V(map->md.cr3);
		uint64_t *kpml4 = P2V(kernel_process.map->md.cr3);
		pte_set(&pml4[i], kpml4[i], kMMUDefaultProt);
	}
}

void
pmap_invlpg(vaddr_t addr)
{
	asm volatile("invlpg %0" : : "m"(*((const char *)addr)) : "memory");
}

vaddr_t invlpg_addr;
volatile atomic_int invlpg_done_cnt;

void
pmap_global_invlpg(vaddr_t vaddr)
{
#if 0
	ipl_t ipl = splraise(kIPLHigh);

	invlpg_addr = vaddr;
	invlpg_done_cnt = 1;
	for (int i = 0; i < ncpus; i++) {
		if (all_cpus[i] == curcpu())
			continue;

		md_ipi_invlpg(all_cpus[i]);
	}
	pmap_invlpg(vaddr);
	while (invlpg_done_cnt != ncpus)
		__asm__("pause");

	splx(ipl);
#else
	kfatal("umimplemented\n");
#endif
}
