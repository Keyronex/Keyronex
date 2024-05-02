#include "kdk/amd64.h"
#include "kdk/amd64/regs.h"
#include "kdk/executive.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

typedef uint64_t pml4e_t, pdpte_t, pde_t;

extern vm_procstate_t kernel_procstate;

void
vmp_md_kernel_init(void)
{
	vm_page_t *kernel_table;
	paddr_t kernel_addr;
	vm_page_alloc(&kernel_table, 0, kPageUsePML4, true);
	kernel_table->process = &kernel_process;
	kernel_addr = vmp_page_paddr(kernel_table);
	kernel_procstate.md.table = kernel_addr;
	memcpy((void *)P2V(kernel_addr), (void *)P2V(read_cr3()), PGSIZE);
	write_cr3(kernel_addr);
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

		r = vmp_fetch_pte(kernel_process.vm, PGROUNDDOWN(addr), &pte);
		kassert(r == 0);
		paddr = vmp_pte_hw_paddr(pte, 1);
		paddr += addr % PGSIZE;
	}

	return paddr;
}

void
vmp_md_enter_kwired(void)
{
	kfatal("Implement me\n");
}

void
vmp_md_setup_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    vm_page_t *tablepage, pte_t *dirpte, bool is_new)
{
	pte_t pte;
	pte.value = 0x0;
	pte.hw.valid = 1;
	pte.hw.writeable = 1;
	pte.hw.user = 1;
	pte.hw.pfn = tablepage->pfn;
	dirpte->value = pte.value;
	vmp_pagetable_page_noswap_pte_created(ps, dirpage, true);
}

void
vmp_md_delete_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage, pte_t *dirpte)
{
	dirpte->hw.value = 0x0;
	vmp_pagetable_page_pte_deleted(ps, dirpage, false);
}
