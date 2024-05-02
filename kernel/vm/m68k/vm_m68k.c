#include "kdk/libkern.h"
#include "kdk/m68k.h"
#include "kdk/nanokern.h"
#include "kdk/port.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kdk/executive.h"
#include "kdk/vmem_impl.h"
#include "mmu_040.h"
#include "vm/m68k/vmp_m68k.h"
#include "vm/vmp.h"

#define kPTEWireStatePML2 1
#define kPTEWireStatePML1 0

const char *vm_page_use_str(enum vm_page_use use);

extern vm_procstate_t kernel_procstate;

static void
store_urp_and_srp(paddr_t val)
{
	asm volatile("movec %0, %%urp\n\t"
		     "movec %0, %%srp"
		     :
		     : "r"(val));
}

static paddr_t
fetch_urp(void)
{
	paddr_t urp;
	asm volatile("movec %%urp, %0" : "=r"(urp));
	return urp;
}

void
vmp_md_kernel_init(void)
{
	vm_page_t *kernel_table;
	paddr_t kernel_addr;
	vm_page_alloc(&kernel_table, 0, kPageUsePML3, true);
	kernel_table->process = &kernel_process;
	kernel_addr = vmp_page_paddr(kernel_table);
	kernel_procstate.md.table = kernel_addr;
	memcpy((void *)P2V(kernel_addr), (void *)P2V(fetch_urp()), PGSIZE);
	store_urp_and_srp(kernel_addr);
}

paddr_t
vmp_md_translate(vaddr_t addr)
{
	uint32_t mmusr;

	asm volatile("ptestw (%0)" : : "a"(addr));
	asm volatile("movec %%mmusr,%0" : "=d"(mmusr));

	union {
		struct {
			uint32_t phys : 20, buserr : 1, global : 1, u1 : 1,
			    u0 : 1, supervisor : 1, cachemode : 2, modified : 1,
			    o : 1, writeprotect : 1, transparent : 1,
			    resident : 1;
		};
		uint32_t val;
	} stuff;

	stuff.val = mmusr;
	kassert(!stuff.buserr);
	return PFN_TO_PADDR(stuff.phys) + addr % PGSIZE;
}

void
vmp_md_setup_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage,
    vm_page_t *tablepage, pte_t *dirpte, bool is_new)
{
	int npages;
	paddr_t phys = vm_page_paddr(tablepage);

	if (dirpage->use == kPageUsePML3)
		npages = 8;
	else if (dirpage->use == kPageUsePML2)
		npages = 16;
	else
		kfatal("unexpected page directory use\n");

	/* retain the page directory, and update its PTE counts accordingly...
	 */
	vmp_page_retain_locked(dirpage);
	/* add remainder of refcount... */
	dirpage->refcnt += npages - 1;
	dirpage->noswap_ptes += npages;
	dirpage->nonzero_ptes += npages;

	dirpte = (pte_t *)ROUNDDOWN(dirpte, npages * sizeof(pte_t));

	for (int i = 0; i < npages; i++) {
		pte_t pte;
		pte.hw_pml2_040.addr = (phys + i * PGSIZE / npages) >> 4;
		pte.hw_pml2_040.used = 0;
		pte.hw_pml2_040.writeprotect = 0;
		pte.hw_pml2_040.type = 3;
		dirpte[i].value = pte.value;
	}
}

void
vmp_md_delete_table_pointers(vm_procstate_t *ps, vm_page_t *dirpage, pte_t *dirpte)
{
	int npages;

	if (dirpage->use == kPageUsePML3)
		npages = 8;
	else if (dirpage->use == kPageUsePML2)
		npages = 16;
	else
		kfatal("unexpected page directory use\n");

	dirpte = (pte_t *)ROUNDDOWN(dirpte, npages * sizeof(pte_t));

	for (int i = 0; i < npages; i++)
		dirpte[i].value = 0x0;

	/* TODO: update pagetable_page_pte_deleted to take a count */
	dirpage->refcnt -= npages - 1;
	dirpage->noswap_ptes -= npages - 1;
	dirpage->nonzero_ptes -= npages - 1;

	/* carry out the final deletion */
	vmp_pagetable_page_pte_deleted(ps, dirpage, false);
}
