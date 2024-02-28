#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "vmp.h"

int vmp_md_enter_kwired(vaddr_t virt, paddr_t phys);
vm_page_t *vmp_md_unenter_kwired(vaddr_t virt);

extern vm_procstate_t kernel_procstate;

int
internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out)
{
	int r;

	r = vmem_xalloc(vmem, size, 0, 0, 0, 0, 0, flags | kVMemPFNDBHeld, out);
	if (r < 0) {
		kfatal("vmem_xalloc returned %d\n", r);
		return r;
	}

#ifdef TRACE_KWIRED
	kprintf("Entering from %p-%p (size %zu)\n", *out,
	    (*out) + size, size);
#endif
	for (int i = 0; i < size - 1; i += PGSIZE) {
		vm_page_t *page;
		vmp_page_alloc_locked(&page, kPageUseKWired, true);
		vmp_md_enter_kwired(*out + i, vm_page_paddr(page));
	}

	return 0;
}

void
internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags)
{
	int r;

#if 0

	r = vmem_xfree(vmem, addr, size, flags | kVMemPFNDBHeld);
	if (r < 0) {
		kfatal("internal_freewired: vmem returned %d\n", r);
		return;
	}
	r = size;

#ifdef TRACE_KWIRED
	kprintf("Unentering from %p-%p (size %zu)\n", addr, addr * size,
	    size);
#endif
	for (int i = 0; i < r; i += PGSIZE) {
		vm_page_t *page = vmp_md_unenter_kwired((vaddr_t)addr + i);
		(void)page;
		pmap_invlpg(addr + i);
		// kassert(page->wirecnt == 1);
		// page->wirecnt = 0;
		// vmp_page_free_locked(kernel_process.map, page);
	}
#endif
}

vaddr_t
vm_kalloc(size_t npages, vmem_flag_t flags)
{
	vmem_addr_t addr;
	int r;
	ipl_t ipl;

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();
	if (npages > 1) {
		r = vmem_xalloc(&vmem_kern_nonpaged, npages * PGSIZE, 0, 0, 0,
		    0, 0, flags | kVMemPFNDBHeld, &addr);
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
#if 1
	ipl_t ipl;

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();

	if (npages > 1) {
		vmem_xfree(&vmem_kern_nonpaged, (vmem_addr_t)addr,
		    npages * PGSIZE, flags | kVMemPFNDBHeld);
	} else {
	}

	if (!(flags & kVMemPFNDBHeld))
		vmp_release_pfn_lock(ipl);
#endif
}

int
vm_allocspace(size_t npages, size_t align, vaddr_t *out)
{
	ipl_t ipl;
	int r;

	ipl = vmp_acquire_pfn_lock();
	r = vmem_xalloc(&kernel_procstate.vmem, npages * PGSIZE, align, 0, 0, 0,
	    0, kVMemPFNDBHeld, out);
	vmp_release_pfn_lock(ipl);

	return r;
}
