#include "kdk/amd64.h"
#include "kdk/amd64/regs.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

typedef uint64_t pml4e_t, pdpte_t, pde_t;

extern vm_procstate_t kernel_procstate;

void
vmp_md_kernel_init(void)
{
	vm_page_t *kernel_table;
	paddr_t kernel_addr;
	vm_page_alloc(&kernel_table, 0, kPageUsePML3, true);
	kernel_addr = vmp_page_paddr(kernel_table);
	kernel_procstate.md.table = kernel_addr;
	memcpy((void *)P2V(kernel_addr), (void *)P2V(read_cr3()), PGSIZE);
}

paddr_t
vmp_md_translate(vaddr_t addr)
{
	kfatal("Implement me\n");
}

void
pmap_invlpg(vaddr_t addr)
{
	asm volatile("invlpg %0" : : "m"(*((const char *)addr)) : "memory");
}


void vmp_md_enter_kwired(void)
{
	kfatal("Implement me\n");
}

void
vmp_md_setup_table_pointers(kprocess_t *ps, vm_page_t *dirpage,
    vm_page_t *tablepage, pte_t *dirpte, bool is_new)
{
	kfatal("Implement me\n");
}

void
vmp_md_delete_table_pointers(kprocess_t *ps, vm_page_t *dirpage, pte_t *pte)
{
	kfatal("Implement me\n");
}
