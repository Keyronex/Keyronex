/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#include "kdk/executive.h"
#include "vm/vmp.h"

void
vmp_md_ps_init(eprocess_t *ps)
{
	vm_procstate_t *vmps = ps->vm;
	vm_page_t *table_page;
	paddr_t table_addr;
#if 0
	pte_t *table_ptebase;
#endif

	vm_page_alloc(&table_page, 0, kPageUsePML4, true);
	table_page->process = ps;
	table_addr = vmp_page_paddr(table_page);
	vmps->md.table = table_addr;

	for (;;) ;

#if 0
	if (vmps == &kernel_procstate) {
		struct id_aa64mmfr0_el1 id = read_id_aa64mmfr0_el1();
		struct tcr_el1 tcr = read_tcr_el1();

		if (id.tgran4 != 0) {
			kfatal("todo: support non 4kib-granularity pages\n")
		} else if (tcr.tg0 != kTG0GranuleSize4KiB ||
		    tcr.tg1 != kTG1GranuleSize4KiB) {
			kfatal("todo: support non 4kib-granularity pages\n")
		} else if (tcr.t0sz != 16 || tcr.t1sz != 16) {
			kfatal("todo: support non-16 t0sz/t1sz\n");
		}

		memcpy((void *)P2V(table_addr), (void *)P2V(read_ttbr1_el1()),
		    PGSIZE);


#if 0 /* unneeded on aarch64! */
		/* preallocate higher half entries */
		table_ptebase = (pte_t *)P2V(table_addr);
		for (int i = 256; i < 512; i++) {
			pte_t *pte = &table_ptebase[i];
			if (pte->value == 0) {
				vm_page_t *pml3_page;

				vm_page_alloc(&pml3_page, 0, kPageUsePML3,
				    true);
				pml3_page->process = ps;
				pml3_page->nonzero_ptes = 10000;
				pml3_page->noswap_ptes = 10000;
				pml3_page->refcnt = 10000;
				pte->hw_table.valid = 1;
				pte->hw_table.is_table = 1;
				pte->hw_table.next_level = pml3_page->pfn;
			}
		}
#endif

	write_ttbr1_el1(table_addr);
	}
#endif
}

paddr_t
vmp_md_translate(vaddr_t addr)
{
	pte_t *pte;
	paddr_t paddr;

	if (addr >= HHDM_BASE && addr <= HHDM_BASE + HHDM_SIZE)
		paddr = V2P(addr);
	else {
		int r;

		r = vmp_fetch_pte(kernel_process->vm, PGROUNDDOWN(addr), &pte);
		kassert(r == 0);
		paddr = vmp_pte_hw_paddr(pte, 1);
		paddr += addr % PGSIZE;
	}

	return paddr;
}

void
vmp_md_setup_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    vm_page_t *tablepage, pte_t *dirpte, enum vmp_table_old_state old_state)
{
	pte_t pte;
	pte.value = 0x0;
	pte.hw.valid = 1;
	pte.hw.pfn = tablepage->pfn;
	dirpte->value = pte.value;

	if (old_state != kWasTrans)
		vmp_pagetable_page_noswap_pte_created(ps, dirpage,
		    old_state == kWasZero ? true : false);
}

void
vmp_md_busy_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte, vm_page_t *tablepage)
{
	vmp_md_pte_create_busy(dirpte, tablepage->pfn);
	vmp_pagetable_page_noswap_pte_created(ps, dirpage, true);
}

void
vmp_md_trans_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte, vm_page_t *tablepage)
{
	vmp_md_pte_create_trans(dirpte, tablepage->pfn);
}

void
vmp_md_swap_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte, uintptr_t drumslot)
{
	vmp_md_pte_create_swap(dirpte, drumslot);
	vmp_pagetable_page_pte_became_swap(ps, dirpage);
}

void
vmp_md_delete_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    pte_t *dirpte)
{
	dirpte->value = 0x0;
	vmp_pagetable_page_pte_deleted(ps, dirpage, false);
}
