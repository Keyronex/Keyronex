#include "kdk/libkern.h"
#include "kdk/m68k.h"
#include "kdk/nanokern.h"
#include "kdk/port.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kdk/vmem_impl.h"
#include "mmu.h"
#include "vm/vmp.h"

#define kPTEWireStatePML2 1
#define kPTEWireStatePML1 0

union m68040_addr {
	struct {
		uint32_t l3i : 7, l2i : 7, l1i : 6, pgi : 12;
	};
	uint32_t addr;
};

void vmem_earlyinit(void);
int internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out);
void internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags);

vm_procstate_t kernel_procstate;
vmem_t vmem_kern_nonpaged;

static paddr_t
fetch_urp(void)
{
	paddr_t urp;
	asm volatile("movec %%urp, %0" : "=r"(urp));
	return urp;
}

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
	kernel_procstate.md.table = fetch_urp();
	RB_INIT(&kernel_procstate.vad_queue);
	RB_INIT(&kernel_procstate.wsl.tree);
	TAILQ_INIT(&kernel_procstate.wsl.queue);
}

/*!
 * \pre PFN DB locked.
 */
int
pmap_get_pte_ptr(void *pmap, vaddr_t vaddr, pte_hw_t **out,
    vm_page_t **out_page)
{
	union m68040_addr addr;

	pml3e_t *pml3_phys = (void *)fetch_urp(),
		*pml3_virt = (void *)P2V(pml3_phys);
	vm_page_t *pml2_page, *pml1_page;

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	addr.addr = (uint32_t)vaddr;
	if (pml3_virt[addr.l3i].addr == 0) {
		int r = vmp_page_alloc_locked(&pml2_page, kPageUsePML2, false);
		int l3i_base = ROUNDDOWN(addr.l3i, 8);
		uintptr_t pml2s_phys;

		if (r != 0)
			return r;

#if VM_DEBUG_PAGETABLES
		pac_printf("Allocated page (Paddr 0x%zx) for PML2s\n",
		    vm_page_paddr(pml2_page));
#endif

		pml2_page->refcnt = 0;
		pml2_page->nonzero_ptes = 0;
		pml2_page->valid_ptes = 0;
		pml2_page->referent_pte = (paddr_t)&pml3_phys[l3i_base];

		pml2s_phys = vm_page_paddr(pml2_page);
		for (int i = 0; i < 8; i++) {
			pml3_virt[l3i_base + i].addr = (pml2s_phys + i * 512) >>
			    4;
			pml3_virt[l3i_base + i].used = 0;
			pml3_virt[l3i_base + i].writeprotect = 0;
			pml3_virt[l3i_base + i].type = 3;
		}
	}

	pml2e_t *pml2_phys = (void *)(pml3_virt[addr.l3i].addr << 4),
		*pml2_virt = (void *)P2V(pml2_phys);
	pml2_page = vm_paddr_to_page((paddr_t)pml2_phys);

	if (pml2_virt[addr.l2i].addr == 0) {
		int r = vmp_page_alloc_locked(&pml1_page, kPageUsePML1, false);
		int l2i_base = ROUNDDOWN(addr.l2i, 16);
		uintptr_t pml1s_phys;

		if (r != 0)
			return r;

#if 1 // VM_DEBUG_PAGETABLES
		pac_printf("Allocated page (Paddr 0x%zx) for PML1s\n",
		    vm_page_paddr(pml1_page));
#endif

		pml2_page->refcnt += 16;
		pml2_page->nonzero_ptes += 16;
		pml2_page->valid_ptes += 16;

		pml1_page->refcnt = 0;
		pml1_page->nonzero_ptes = 0;
		pml1_page->valid_ptes = 0;
		pml1_page->referent_pte = (paddr_t)&pml2_phys[l2i_base];

		pml1s_phys = vm_page_paddr(pml1_page);
		for (int i = 0; i < 16; i++) {
			pml2_virt[l2i_base + i].addr = (pml1s_phys + i * 256) >>
			    4;
			pml2_virt[l2i_base + i].used = 0;
			pml2_virt[l2i_base + i].writeprotect = 0;
			pml2_virt[l2i_base + i].type = 3;
		}
	}

	pte_hw_t *pml1 = (void *)(pml2_virt[addr.l2i].addr << 4);
	*out = (pte_hw_t*)P2V(&pml1[addr.l1i]);
	*out_page = vm_paddr_to_page(PGROUNDDOWN(pml1));

	return 0;
}

static void
free_pagetable(vm_procstate_t *vmps, vm_page_t *page)
{
#if VM_DEBUG_PAGETABLES
	pac_printf("Pagetable page 0x%zx to be freed. Its referent PTE is %p. "
		   "Its parent pagetable is 0x%zx\n",
	    vm_page_paddr(page), page->referent_pte,
	    vm_page_paddr(parent_page));
#endif

	if (page->use == kPageUsePML1 /*|| level == 2 */) {
		vm_page_t *parent_page;
		uint32_t *referent_pte_phys = (void *)page->referent_pte,
			 *referent_pte_virt = (void *)P2V(referent_pte_phys);
		parent_page = vm_paddr_to_page(PGROUNDDOWN(referent_pte_phys));
		int nentries = page->use == kPageUsePML1 ? 16 : 8;

		/* clear all PTEs referring to this page */
		for (int i = 0; i < nentries; i++) {
			*referent_pte_virt++ = 0x0;
			vmp_md_pagetable_pte_zeroed(vmps, parent_page);
		}
	} else if (page->use == kPageUsePML2) {
		/* we can't get parent page yet */
		uint32_t *referent_pte_phys = (void *)page->referent_pte,
			 *referent_pte_virt = (void *)P2V(referent_pte_phys);
		int nentries = 8;

		/* clear all PTEs referring to this page */
		for (int i = 0; i < nentries; i++)
			*referent_pte_virt++ = 0x0;
	} else {
		kfatal("free_pagetable: Unexpected level %d\n", page->use);
	}

#if 0
	vmp_page_delete_locked(page, &vmps->account, true);
#endif
}

int
vmp_md_enter_kwired(vaddr_t virt, paddr_t phys)
{
	pte_hw_t *ppte;
	vm_page_t *ppagetablepage;
	int r;

	r = pmap_get_pte_ptr(0, virt, &ppte, &ppagetablepage);
	if (r < 0)
		return r;

	if (ppte->type != 0) {
		kfatal("entering virt address 0x%zx (map to 0x%zx): "
		       "pte not invalid, is mapped to 0x%zx", virt, phys,
		    PFN_TO_PADDR(ppte->pfn));
	}

	ppagetablepage->refcnt += 1;
	ppagetablepage->nonzero_ptes += 1;
	ppagetablepage->valid_ptes += 1;

	ppte->pfn = (uintptr_t)phys >> 12;
	ppte->cachemode = 1; /* cacheable, copyback */
	ppte->supervisor = 1;
	ppte->type = 3;
	ppte->global = 1;
	ppte->writeprotect = 0;

	return 0;
}

int
vmp_md_unenter_kwired(vaddr_t virt)
{
	pte_hw_t *ppte;
	vm_page_t *ppagetablepage;
	int r;

	r = pmap_get_pte_ptr(0, virt, &ppte, &ppagetablepage);
	if (r < 0)
		return r;

	kassert(ppte->type == 3);

	memset(ppte, 0x0, sizeof(pte_hw_t));
	vmp_md_pagetable_pte_zeroed(&kernel_procstate, ppagetablepage);

	return 0;
}

/* NOTE: please rewrite this */
vm_fault_return_t
vmp_md_wire_pte(vm_procstate_t *vmps, struct vmp_pte_wire_state *state)
{
	union m68040_addr addr;
	vm_page_t *pml2_page, *pml1_page;

	kassert(ke_spinlock_held(&vmp_pfn_lock));

	addr.addr = state->addr;

	kprintf("Wire PTE for 0x%x: %d.%d.%d\n", addr.addr, addr.l3i, addr.l2i,
	    addr.l1i);

	if (state->pgtable_pages[kPTEWireStatePML1] != NULL)
		goto fetch_pte;
	else if (state->pgtable_pages[kPTEWireStatePML2] != NULL)
		goto fetch_pml1;

	pml3e_t *pml3_phys = (void *)vmps->md.table,
		*pml3_virt = (void *)P2V(pml3_phys);

	if (pml3_virt[addr.l3i].addr == 0) {
		int r = vmp_page_alloc_locked(&pml2_page, kPageUsePML2, false);
		int l3i_base = ROUNDDOWN(addr.l3i, 8);
		uintptr_t pml2s_phys;

		if (r != 0)
			return r;

		pml2_page->valid_ptes = 0;
		pml2_page->nonzero_ptes = 0;
		pml2_page->referent_pte = (paddr_t)&pml3_phys[l3i_base];

		pml2s_phys = vm_page_paddr(pml2_page);
		for (int i = 0; i < 8; i++) {
			pml3_virt[l3i_base + i].addr = (pml2s_phys + i * 512) >>
			    4;
			pml3_virt[l3i_base + i].used = 0;
			pml3_virt[l3i_base + i].writeprotect = 0;
			pml3_virt[l3i_base + i].type = 3;
		}

		state->pgtable_pages[kPTEWireStatePML2] = pml2_page;
	} else {
		/* assuming it's valid... */
		pml2e_t *pml2_phys = (void *)(pml3_virt[addr.l3i].addr << 4);
		pml2_page = vm_paddr_to_page((paddr_t)pml2_phys);
		pml2_page->refcnt++;
		state->pgtable_pages[kPTEWireStatePML2] = pml2_page;
	}

fetch_pml1:
	pml2_page = state->pgtable_pages[kPTEWireStatePML2];
	pml2e_t *pml2_phys = (void *)(pml3_virt[addr.l3i].addr << 4),
		*pml2_virt = (void *)P2V(pml2_phys);

	if (pml2_virt[addr.l2i].addr == 0) {
		int r = vmp_page_alloc_locked(&pml1_page, kPageUsePML1, false);
		int l2i_base = ROUNDDOWN(addr.l2i, 16);
		uintptr_t pml1s_phys;

		if (r != 0)
			return r;

		pml2_page->refcnt += 16;
		pml2_page->nonzero_ptes += 16;
		pml2_page->valid_ptes += 16;

		pml1_page->referent_pte = (paddr_t)&pml2_phys[l2i_base];
		pml1_page->valid_ptes = 0;
		pml1_page->nonzero_ptes = 0;

		pml1s_phys = vm_page_paddr(pml1_page);
		for (int i = 0; i < 16; i++) {
			pml2_virt[l2i_base + i].addr = (pml1s_phys + i * 256) >>
			    4;
			pml2_virt[l2i_base + i].used = 0;
			pml2_virt[l2i_base + i].writeprotect = 0;
			pml2_virt[l2i_base + i].type = 3;
		}
		state->pgtable_pages[kPTEWireStatePML1] = pml1_page;
	} else {
		/* assuming it's valid... */
		pte_hw_t *pml1_phys = (void *)(pml2_virt[addr.l2i].addr << 4);
		pml1_page = vm_paddr_to_page((paddr_t)pml1_phys);
		pml1_page->refcnt++;
		state->pgtable_pages[kPTEWireStatePML1] = pml1_page;
	}

fetch_pte:
	pml1_page = state->pgtable_pages[kPTEWireStatePML1];
	pml1_page->nonzero_ptes++;
	pml1_page->valid_ptes++;
	pte_hw_t *pml1 = (void *)(pml2_virt[addr.l2i].addr << 4);
	state->pte = (pte_t *)P2V(&pml1[addr.l1i]);

	return kVMFaultRetOK;
}

void
vmp_md_pagetable_pte_zeroed(vm_procstate_t *vmps, vm_page_t *page)
{
	page->valid_ptes--;
	page->nonzero_ptes--;
	if (page->nonzero_ptes == 0)
		free_pagetable(vmps, page);
	else
		vmp_page_release_locked(page);
}

void
vmp_md_pagetable_ptes_created(struct vmp_pte_wire_state *state, size_t count)
{
	state->pgtable_pages[kPTEWireStatePML1]->valid_ptes += count;
	state->pgtable_pages[kPTEWireStatePML1]->nonzero_ptes += count;
	state->pgtable_pages[kPTEWireStatePML1]->refcnt += count;
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

void pmap_invlpg(vaddr_t addr)
{
	asm volatile ("pflush (%0)" : : "a"(addr));
}
