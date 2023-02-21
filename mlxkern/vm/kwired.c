/*
 * Copyright (c) 2022-2023 The Melantix Project.
 * Created in 2022.
 */
/**
 * @file vm_kernel.c
 * @brief Management of the kernel's virtual address space, especially wired.
 */

#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kdk/vmem_impl.h"
#include "vm_internal.h"

/* Kernel wired heap arena. */
vmem_t vm_kernel_wired;

static int
internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out)
{
	int r;
	ipl_t ipl;

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();

	kassert(vmem == &kernel_process.vmps.vmem);

	r = vmem_xalloc(vmem, size, 0, 0, 0, 0, 0, flags, out);
	if (r < 0) {
		kfatal("vmem_xalloc returned %d\n", r);
		return r;
	}

	for (int i = 0; i < size - 1; i += PGSIZE) {
		vm_page_t *page;
		vmp_page_alloc(&kernel_process.vmps, true, kPageUseWired,
		    &page);
		pmap_enter(&kernel_process.vmps, page->address,
		    (vaddr_t)*out + i, kVMAll);
	}

	if (!(flags & kVMemPFNDBHeld))
		vmp_release_pfn_lock(ipl);

	return 0;
}

static void
internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size,
    vmem_flag_t flags)
{
	int r;
	ipl_t ipl;

	kassert(vmem == &kernel_process.vmps.vmem);

	if (!(flags & kVMemPFNDBHeld))
		ipl = vmp_acquire_pfn_lock();

	r = vmem_xfree(vmem, addr, size, flags);
	if (r < 0) {
		kdprintf("internal_freewired: vmem returned %d\n", r);
		vmp_release_pfn_lock(ipl);
		return;
	}
	r = size;

	for (int i = 0; i < r; i += PGSIZE) {
		vm_page_t *page;
		page = pmap_unenter(&kernel_process.vmps, (vaddr_t)addr + i);
		kassert(page->reference_count == 1);
		page->reference_count = 0;
		vmp_page_free(&kernel_process.vmps, page);
	}

	if (!(flags & kVMemPFNDBHeld))
		vmp_release_pfn_lock(ipl);
}

void
vm_kernel_dump()
{
	vmem_dump(&kernel_process.vmps.vmem);
	vmem_dump(&vm_kernel_wired);
}

void
vmp_kernel_init()
{
	vmem_earlyinit();
	vmem_init(&kernel_process.vmps.vmem, "kernel-va", KHEAP_BASE,
	    KHEAP_SIZE, PGSIZE, NULL, NULL, NULL, 0, kVMemBootstrap, 0);
	vmem_init(&vm_kernel_wired, "kernel-wired", 0, 0, PGSIZE,
	    internal_allocwired, internal_freewired, &kernel_process.vmps.vmem,
	    0, kVMemBootstrap, 0);

	kernel_process.vmps.vmem.flags = 0;
	vm_kernel_wired.flags = 0;
}

vaddr_t
vm_kalloc(size_t npages, vmem_flag_t flags)
{
	vmem_addr_t addr;
	int r;

	r = vmem_xalloc(&vm_kernel_wired, npages * PGSIZE, 0, 0, 0, 0, 0, flags,
	    &addr);
	kassert(r == 0);
	if (r == 0)
		return (vaddr_t)addr;
	else
		return (vaddr_t)NULL;
}

void
vm_kfree(vaddr_t addr, size_t npages, vmem_flag_t flags)
{
	vmem_xfree(&vm_kernel_wired, (vmem_addr_t)addr, npages * PGSIZE, flags);
}
