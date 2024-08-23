#include "kdk/executive.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kern/ki.h"
#include "vmp.h"

int vmp_md_enter_kwired(vaddr_t virt, paddr_t phys);
vm_page_t *vmp_md_unenter_kwired(vaddr_t virt);

void vmem_earlyinit(void);
int internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out);
void internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags);

vm_procstate_t kernel_procstate;
vmem_t vmem_kern_nonpaged_va;

void
vmp_kernel_init(void)
{
	vmem_earlyinit();
	vmem_init(&vmem_kern_nonpaged_va, "kernel-nonpaged-va", KVM_WIRED_BASE,
	    KVM_WIRED_SIZE, PGSIZE, NULL, NULL, NULL, 0, kVMemBootstrap, kIPL0);
	vm_ps_init(kernel_process);
}

int
vmp_enter_kwired(vaddr_t virt, paddr_t phys)
{
	int r;
	struct vmp_pte_wire_state pte_wire;

	r = vmp_wire_pte(kernel_process, virt, 0, &pte_wire, true);
	kassert(r == 0);

	vmp_md_pte_create_hw(pte_wire.pte, phys >> VMP_PAGE_SHIFT, true, true,
	    true, false);
	vmp_pagetable_page_noswap_pte_created(kernel_process->vm,
	    pte_wire.pgtable_pages[0], true);
	vmp_pte_wire_state_release(&pte_wire, false);

	return 0;
}

int
internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out)
{
	int r;
	pte_t *pte = NULL;
	struct vmp_pte_wire_state pte_wire;
	vmem_addr_t addr;

	r = vmem_xalloc(vmem, size, 0, 0, 0, 0, 0, flags | kVMemPFNDBHeld,
	    &addr);
	if (r < 0) {
		kfatal("vmem_xalloc returned %d\n", r);
		return r;
	}

	*out = addr;

#ifdef TRACE_KWIRED
	kprintf("Entering from %p-%p (size %zu)\n", *out, (*out) + size, size);
#endif

	for (int i = 0; i < size - 1; i += PGSIZE, addr += PGSIZE) {
		vm_page_t *page;

		if (i == 0 || ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
			if (pte)
				vmp_pte_wire_state_release(&pte_wire, false);
			r = vmp_wire_pte(kernel_process, addr, 0, &pte_wire,
			    true);
			kassert(r == 0);
			pte = pte_wire.pte;
		}

		r = vmp_page_alloc_locked(&page, kPageUseKWired, true);
		kassert(r == 0);
		vmp_md_pte_create_hw(pte, vm_page_pfn(page), true, true, true,
		    false);
		vmp_pagetable_page_noswap_pte_created(kernel_process->vm,
		    pte_wire.pgtable_pages[0], true);
	}

	vmp_pte_wire_state_release(&pte_wire, false);

	return 0;
}

void
internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags)
{
#if 1
	int r;
	pte_t *pte = NULL;
	struct vmp_pte_wire_state pte_wire;

#if 0
	r = vmem_xfree(vmem, addr, size, flags | kVMemPFNDBHeld);
	if (r < 0) {
		kfatal("internal_freewired: vmem returned %d\n", r);
		return;
	}
#endif
	r = size;

#ifdef TRACE_KWIRED
	kprintf("Unentering from %p-%p (size %zu)\n", addr, addr * size, size);
#endif
	for (int i = 0; i < size - 1; i += PGSIZE, addr += PGSIZE) {
		vm_page_t *page;

		if (i == 0 || ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
			if (pte)
				vmp_pte_wire_state_release(&pte_wire, false);
			r = vmp_wire_pte(kernel_process, addr, 0, &pte_wire,
			    true);
			kassert(r == 0);
			pte = pte_wire.pte;
		}

		page = vmp_pte_hw_page(pte, 1);
		pte->value = 0x0;
		/*! TODO: urgent! why is this not global?! */
		ki_tlb_flush_vaddr_locally(addr);
		vmp_pagetable_page_pte_deleted(&kernel_procstate,
		    pte_wire.pgtable_pages[0], false);
		vmp_page_delete_locked(page);
		vmp_page_release_locked(page);
	}
	vmp_pte_wire_state_release(&pte_wire, false);
#endif
}

int
internal_reallocwired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t oldsize,
    vmem_size_t newsize, vmem_flag_t flags, vmem_addr_t *out)
{
	int r;
	pte_t *pte = NULL;
	struct vmp_pte_wire_state pte_wire;
	vmem_addr_t new_addr;

	r = vmem_xrealloc(vmem, addr, oldsize, newsize, flags | kVMemPFNDBHeld,
	    &new_addr);
	if (r < 0) {
		kfatal("vmem_xrealloc returned %d\n", r);
		return r;
	}

	*out = new_addr;

	if (newsize > oldsize) {
		addr = new_addr + oldsize;
		for (int i = oldsize; i < newsize;
		     i += PGSIZE, addr += PGSIZE) {
			vm_page_t *page;

			if (i == oldsize ||
			    ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
				if (pte)
					vmp_pte_wire_state_release(&pte_wire,
					    false);
				r = vmp_wire_pte(kernel_process, addr, 0,
				    &pte_wire, true);
				kassert(r == 0);
				pte = pte_wire.pte;
			}

			r = vmp_page_alloc_locked(&page, kPageUseKWired, true);
			kassert(r == 0);
			vmp_md_pte_create_hw(pte, vm_page_pfn(page), true, true,
			    true, false);
			vmp_pagetable_page_noswap_pte_created(
			    kernel_process->vm, pte_wire.pgtable_pages[0],
			    true);
		}
		vmp_pte_wire_state_release(&pte_wire, false);
	} else if (newsize < oldsize) {
		addr = new_addr + newsize;
		for (int i = newsize; i < oldsize;
		     i += PGSIZE, addr += PGSIZE) {
			vm_page_t *page;

			if (i == newsize ||
			    ((uintptr_t)(++pte) & (PGSIZE - 1)) == 0) {
				if (pte)
					vmp_pte_wire_state_release(&pte_wire,
					    false);
				r = vmp_wire_pte(kernel_process, addr, 0,
				    &pte_wire, true);
				kassert(r == 0);
				pte = pte_wire.pte;
			}

			page = vmp_pte_hw_page(pte, 1);
			pte->value = 0x0;
			vmp_pagetable_page_pte_deleted(&kernel_procstate,
			    pte_wire.pgtable_pages[0], false);
			vmp_page_delete_locked(page);
			vmp_page_release_locked(page);
		}
		vmp_pte_wire_state_release(&pte_wire, false);
	}

	return 0;
}

vaddr_t
vm_kalloc(size_t npages, vmem_flag_t flags)
{
	vmem_addr_t addr;
	int r;
	ipl_t ipl = kIPL0; /* only to silence  -Wmaybe-uninitialized */

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();
	if (npages > 1) {
		r = internal_allocwired(&vmem_kern_nonpaged_va, npages * PGSIZE,
		    flags | kVMemPFNDBHeld, &addr);
		if (r != 0) /* xxx release pfn lock!! */ {
			kfatal("Failed\n");
			return 0;
		}
	} else {
		vm_page_t *page;

		r = vmp_page_alloc_locked(&page, kPageUseKWired, true);
		if (r != 0) /* xxx release pfn lock! */ {
			kfatal("failed\n");
			return 0;
		}

		addr = vm_page_direct_map_addr(page);
	}
	if (!(flags & kVMemPFNDBHeld))
		vmp_release_pfn_lock(ipl);

	return (vaddr_t)addr;
}

void
vm_kfree(vaddr_t addr, size_t npages, vmem_flag_t flags)
{
	ipl_t ipl = kIPL0; /* only to silence  -Wmaybe-uninitialized */

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();

	if (npages > 1) {
		internal_freewired(&vmem_kern_nonpaged_va, (vmem_addr_t)addr,
		    npages * PGSIZE, flags | kVMemPFNDBHeld);
	} else {
		vm_page_t *page = vm_paddr_to_page(V2P(addr));
		vmp_page_delete_locked(page);
		vmp_page_release_locked(page);
	}

	if (!(flags & kVMemPFNDBHeld))
		vmp_release_pfn_lock(ipl);
}

vaddr_t
vm_krealloc(vaddr_t addr, size_t old_npages, size_t new_npages,
    vmem_flag_t flags)
{
	vmem_addr_t new_addr;
	int r;
	ipl_t ipl = kIPL0; /* only to silence  -Wmaybe-uninitialized */

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();

	if (new_npages != old_npages) {
		r = internal_reallocwired(&vmem_kern_nonpaged_va,
		    (vmem_addr_t)addr, old_npages * PGSIZE, new_npages * PGSIZE,
		    flags | kVMemPFNDBHeld, &new_addr);
		if (r != 0) {
			kfatal("Failed to reallocate\n");
			return 0;
		}
	} else {
		new_addr = addr;
	}

	if (!(flags & kVMemPFNDBHeld))
		vmp_release_pfn_lock(ipl);

	return (vaddr_t)new_addr;
}
