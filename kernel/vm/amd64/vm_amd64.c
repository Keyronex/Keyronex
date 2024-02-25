#include "kdk/amd64.h"
#include "kdk/amd64/regs.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

typedef uint64_t pml4e_t, pdpte_t, pde_t;

void vmem_earlyinit(void);
int internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out);
void internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags);

vm_procstate_t kernel_procstate;
vmem_t vmem_kern_nonpaged;

void
vmp_kernel_init(void)
{
	vmem_earlyinit();

	vmem_init(&kernel_procstate.vmem, "kernel-va", KVM_DYNAMIC_BASE,
	    KVM_DYNAMIC_SIZE, PGSIZE, NULL, NULL, NULL, 0, kVMemBootstrap,
	    kIPL0);
	vmem_init(&vmem_kern_nonpaged, "kernel-nonpaged", 0, 0, PGSIZE,
	    internal_allocwired, internal_freewired, &kernel_procstate.vmem, 0,
	    kVMemBootstrap, kIPL0);

	ke_mutex_init(&kernel_procstate.mutex);
	kernel_procstate.md.table = read_cr3();
	RB_INIT(&kernel_procstate.vad_queue);
	RB_INIT(&kernel_procstate.wsl.tree);
	TAILQ_INIT(&kernel_procstate.wsl.queue);
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

/*!
 * Get the physical address at which a page table entry points. optionally
 * allocate a new entry, setting appropriate flags. Table should be a pointer
 * to the physical location of the table.
 */
static uint64_t *
pmap_descend(void *map, uint64_t *table, size_t index, bool alloc,
    uint64_t mmuprot)
{
	uint64_t *entry = (uint64_t *)P2V((&table[index]));
	uint64_t *addr = NULL;

	if (*entry & kMMUPresent) {
		addr = pte_get_addr(*entry);
	} else if (alloc) {
		vm_page_t *page;
		vmp_page_alloc_locked(&page, &kernel_procstate.account,
		    kPageUsePML3, false);

		addr = (uint64_t *)PFN_TO_PADDR(page->pfn);
		pte_set(entry, (paddr_t)addr, mmuprot);
	}

	return addr;
}

int
vmp_md_enter_kwired(vaddr_t virt, paddr_t phys)
{
	uintptr_t virta = (uintptr_t)virt;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	pml4e_t *pml4 = (pml4e_t *)read_cr3();
	pdpte_t *pdpte;
	pde_t *pde;
	pte_t *pt;
	pte_t *pti_virt;

	pdpte = pmap_descend(NULL, pml4, pml4i, true, kMMUDefaultProt);
	pde = pmap_descend(NULL, pdpte, pdpti, true, kMMUDefaultProt);
	pt = (pte_t *)pmap_descend(NULL, pde, pdi, true, kMMUDefaultProt);

	pti_virt = (pte_t *)P2V(&pt[pti]);
	void *oldaddr;

	uint64_t tib = 1024ul * 1024 * 1024 * 1024;
	kassert(phys < tib);

	if ((oldaddr = pte_get_addr(pti_virt->hw.value)) != NULL) {
		/*! this may turn out to be excessive */
		kfatal("not remapping a PTE without explicit request\n"
		       "(requested vaddr=>phys 0x%lx=>0x%lx\n"
		       "(existing 0x%lx=>%p; PTE is %p)\n",
		    virt, phys, virt, oldaddr, &pt[pti]);
	}

	pte_set((uint64_t*)pti_virt, phys, kMMUPresent | kMMUWrite);
}

int
vmp_md_unenter_kwired(...)
{
	kfatal("Implement...\n");
}

paddr_t
vmp_md_translate(vaddr_t addr)
{
	uint64_t off = addr % PGSIZE;

	if (addr >= HHDM_BASE && addr < HHDM_BASE + HHDM_SIZE)
		return V2P(addr);

	uintptr_t virta = (uintptr_t)addr;
	int pml4i = ((virta >> 39) & 0x1FF);
	int pdpti = ((virta >> 30) & 0x1FF);
	int pdi = ((virta >> 21) & 0x1FF);
	int pti = ((virta >> 12) & 0x1FF);
	pml4e_t *pml4 = (pml4e_t *)read_cr3();
	pdpte_t *pdpte;
	pde_t *pde;
	pte_t *pt;
	pte_t *pti_virt;

	pdpte = pmap_descend(NULL, pml4, pml4i, false, kMMUDefaultProt);
	pde = pmap_descend(NULL, pdpte, pdpti, false, kMMUDefaultProt);
	pt = (pte_t *)pmap_descend(NULL, pde, pdi, false, kMMUDefaultProt);

	pti_virt = (pte_t *)P2V(&pt[pti]);

	return ((paddr_t)pte_get_addr(pti_virt->hw.value)) + off;
}

void
pmap_invlpg(vaddr_t addr)
{
	asm volatile("invlpg %0" : : "m"(*((const char *)addr)) : "memory");
}

void
vmp_md_pagetable_pte_zeroed(vm_procstate_t *vmps, vm_page_t *page)
{
}

void
vmp_md_pagetable_ptes_created(struct vmp_pte_wire_state *state, size_t count)
{
}

int
pmap_get_pte_ptr(void *pmap, vaddr_t vaddr, pte_hw_t **out,
    vm_page_t **out_page)
{
	kfatal("Kindly implement\n");
}
