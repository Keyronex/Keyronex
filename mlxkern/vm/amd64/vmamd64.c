/*
 * Copyright (c) 2022-2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */
/*!
 * @file vm/amd64/vmamd64.c
 * @brief Physical page mapping functionality.
 *
 * Virtually every one of these functions should be called with PFN database
 * lock held.
 */

#include "machdep/amd64/amd64.h"
#include "machdep/machdep.h"
#include "process/ps.h"
#include "vm/vm.h"
#include "vm/vm_internal.h"
#include "vm_md.h"

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

typedef uint64_t pml4e_t, pdpte_t, pde_t, pte_t;

static uint64_t *pte_get_addr(uint64_t pte);
static void pte_set(uint64_t *pte, paddr_t addr, uint64_t flags);

void
pmap_init(void)
{
	uint64_t *cr3 = (void *)read_cr3();
	kernel_process.vmps.md.cr3 = (paddr_t)cr3;

	/* pre-allocate the top 256. they are globally shared. */
	for (int i = 255; i < 511; i++) {
		uint64_t *pml4 = P2V(cr3);
		if (pte_get_addr(pml4[i]) == NULL) {
			vm_page_t *page;
			vmp_page_alloc(&kernel_process.vmps, true, kPageUseVMM,
			    &page);
			pte_set(&pml4[i], page->address, kMMUDefaultProt);
		}
	}
}

/*! Free a level of page tables. \p table is a physical address. */
void
pmap_free_sub(vm_procstate_t *vmps, uint64_t *table, int level)
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
			pmap_free_sub(vmps, pte_get_addr(*entry), level - 1);
		}

	page = vmp_paddr_to_page((uintptr_t)V2P(table));
	vmp_page_free(vmps, page);
}

void
pmap_free(vm_procstate_t *vmps)
{
	uint64_t *vpml4 = P2V(vmps->md.cr3);
	for (int i = 0; i < 255; i++) {
		pte_t *entry = &vpml4[i];
		pmap_free_sub(vmps, pte_get_addr(*entry), 3);
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
	return (prot & kVMRead ? kMMUPresent : 0) |
	    (prot & kVMWrite ? kMMUWrite : 0) | kMMUUser;
}

/*!
 * Get the physical address at which a page table entry points. optionally
 * allocate a new entry, setting appropriate flags. Table should be a pointer
 * to the physical location of the table.
 */
uint64_t *
pmap_descend(vm_procstate_t *vmps, uint64_t *table, size_t index, bool alloc,
    uint64_t mmuprot)
{
	uint64_t *entry = P2V((&table[index]));
	uint64_t *addr = NULL;

	if (*entry & kMMUPresent) {
		addr = pte_get_addr(*entry);
	} else if (alloc) {
		vm_page_t *page;
		vmp_page_alloc(vmps, true, kPageUseVMM, &page);
		addr = (uint64_t *)page->address;
		pte_set(entry, (paddr_t)addr, mmuprot);
	}

	return addr;
}

/*!
 * @returns (physical) pointer to the pte for this virtual address, or NULL if
 * none exists
 */
pte_t *
pmap_fully_descend(vm_procstate_t *vmps, vaddr_t virt)
{
	uintptr_t virta = (uintptr_t)virt;
	pml4e_t *pml4 = (void *)vmps->md.cr3;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	pdpte_t *pdptes;
	pde_t *pdes;
	pte_t *ptes;

	pdptes = pmap_descend(vmps, pml4, pml4i, false, 0);
	if (!pdptes) {
		return 0x0;
	}

	pdes = pmap_descend(vmps, pdptes, pdpti, false, 0);
	if (!pdes) {
		return 0x0;
	}

	ptes = pmap_descend(vmps, pdes, pdi, false, 0);
	if (!ptes) {
		return 0x0;
	}

	return &ptes[pti];
}

paddr_t
pmap_trans(vm_procstate_t *vmps, vaddr_t virt)
{
	uintptr_t virta = (uintptr_t)virt;
	pml4e_t *pml4 = (void *)vmps->md.cr3;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	int pi = ((virta)&0xFFF);

	pdpte_t *pdpte;
	pde_t *pde;
	pte_t *pte;

	pdpte = pmap_descend(vmps, pml4, pml4i, false, 0);
	if (!pdpte) {
		return 0x0;
	}

	pde = pmap_descend(vmps, pdpte, pdpti, false, 0);
	if (!pde) {
		return 0x0;
	}

	pte = pmap_descend(vmps, pde, pdi, false, 0);
	if (!pte) {
		return 0x0;
	}

	pte = P2V(pte);

	if (!(pte[pti] & kMMUPresent))
		return 0x0;
	else
		return (paddr_t)pte_get_addr(pte[pti]) + pi;
}

void
pmap_enter(vm_procstate_t *vmps, paddr_t phys, vaddr_t virt,
    vm_protection_t prot)
{
	uintptr_t virta = (uintptr_t)virt;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	pml4e_t *pml4 = (void *)vmps->md.cr3;
	pdpte_t *pdpte;
	pde_t *pde;
	pte_t *pt;
	pte_t *pti_virt;

	pdpte = pmap_descend(vmps, pml4, pml4i, true, kMMUDefaultProt);
	pde = pmap_descend(vmps, pdpte, pdpti, true, kMMUDefaultProt);
	pt = pmap_descend(vmps, pde, pdi, true, kMMUDefaultProt);

	pti_virt = P2V(&pt[pti]);
	void *oldaddr;

	if ((oldaddr = pte_get_addr(*pti_virt)) != NULL) {
		/*! this may turn out to be excessive */
		kfatal("not remapping a PTE without explicit request\n"
		       "(requested vaddr=>phys 0x%lx=>0x%lx\n"
		       "(existing 0x%lx=>%p; PTE is %p)\n",
		    virt, phys, virt, oldaddr, &pt[pti]);
	}

	pte_set(pti_virt, phys,
	    vm_prot_to_i386(prot) | (virt < KAREA_BASE ? kMMUUser : 0));
}

vm_page_t *
pmap_unenter(vm_procstate_t *vmps, vaddr_t vaddr)
{
	paddr_t paddr;
	pte_t *pte = pmap_fully_descend(vmps, vaddr);

	kassert(pte);
	pte = P2V(pte);
	paddr = (paddr_t)pte_get_addr(*pte);

	kassert(*pte != 0x0);
	*pte = 0x0;

	return vmp_paddr_to_page(paddr);
}

bool
pmap_is_present(vm_procstate_t *vmps, vaddr_t vaddr, paddr_t *paddr)
{
	pte_t *pte = pmap_fully_descend(vmps, vaddr);

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
pmap_is_writeable(vm_procstate_t *vmps, vaddr_t vaddr, paddr_t *paddr)
{
	pte_t *pte = pmap_fully_descend(vmps, vaddr);

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

void
vm_ps_md_init(vm_procstate_t *vmps)
{
	vm_page_t *page;
	vmp_page_alloc(vmps, true, kPageUseVMM, &page);
	vmps->md.cr3 = page->address;
	for (int i = 255; i < 512; i++) {
		uint64_t *pml4 = P2V(vmps->md.cr3);
		uint64_t *kpml4 = P2V(kernel_process.vmps.md.cr3);
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
