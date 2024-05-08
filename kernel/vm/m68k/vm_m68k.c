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

#if 0
{
	vm_procstate_t *vmps = ps->vm;
	vm_page_t *table_page;
	paddr_t table_addr;
	pte_t *table_ptebase;

	vm_page_alloc(&table_page, 0, kPageUsePML4, true);
	table_page->process = ps;
	table_addr = vmp_page_paddr(table_page);
	vmps->md.table = table_addr;

	if (vmps == &kernel_procstate) {
		memcpy((void *)P2V(table_addr), (void *)P2V(read_cr3()),
		    PGSIZE);

		/* preallocate higher half entries */
		table_ptebase = (pte_t *)P2V(table_addr);
		for (int i = 256; i < 512; i++) {
			pte_t *pte = &table_ptebase[i];
			if (pte->value == 0) {
				vm_page_t *pml3_page;

				vm_page_alloc(&pml3_page, 0, kPageUsePML3,
				    true);
				pml3_page->process = &kernel_process;
				pml3_page->nonzero_ptes = 10000;
				pml3_page->noswap_ptes = 10000;
				vmp_md_pte_create_hw(pte, pml3_page->pfn, true,
				    true);
			}
		}

		write_cr3(table_addr);
	} else {

	}
}
#endif

void
vmp_md_ps_init(kprocess_t *ps)
{
	vm_procstate_t *vmps = ps->vm;
	vm_page_t *table_page;
	paddr_t table_addr;
	pte_t *table_ptebase;

	vm_page_alloc(&table_page, 0, kPageUsePML3, true);
	table_page->process = ps;
	table_addr = vmp_page_paddr(table_page);
	vmps->md.table = table_addr;

	if (vmps == &kernel_procstate) {
		memcpy((void *)P2V(table_addr), (void *)P2V(fetch_urp()),
		    PGSIZE);

		/* preallocate higher half entries */
		table_ptebase = (pte_t *)P2V(table_addr);

#if 0
		for (int i = 96; i < 127; i+= 8) {
			pte_t *pte = &table_ptebase[i];
			if (pte->value == 0) {
				vm_page_t *pml3_page;

				vm_page_alloc(&pml3_page, 0, kPageUsePML2,
				    true);
				pml3_page->process = &kernel_process;
				pml3_page->nonzero_ptes = 10000;
				pml3_page->noswap_ptes = 10000;
				vmp_md_pte_create_hw(pte, pml3_page->pfn, true,
				    true);
			}
		}
#else
		(void)table_ptebase;
#endif

		store_urp_and_srp(table_addr);
	} else {
		memcpy((void *)P2V(table_addr + (sizeof(pml3e_040_t) * 64)),
		    (void *)P2V(
			kernel_procstate.md.table + (sizeof(pml3e_040_t) * 64)),
		    (sizeof(pml3e_040_t) * 64));
	}
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
