/**
 * @file vm_kernel.c
 * @brief Management of the kernel's virtual address space, especially wired.
 */

#include <libkern/libkern.h>
#include <vm/vm.h>

#include <kern/vmem.h>
#include <kern/vmem_impl.h>

/** Kernel wired memory. */
vmem_t vm_kernel_wired;

static int
internal_allocwired(vmem_t *vmem, vmem_size_t size, vmem_flag_t flags,
    vmem_addr_t *out)
{
	int r;

	kassert(vmem == &kmap.vmem);

	r = vmem_xalloc(vmem, size, 0, 0, 0, 0, 0, flags, out);
	if (r < 0) {
		kfatal("vmem_xalloc returned %d\n", r);
		return r;
	}

	for (int i = 0; i < size - 1; i += PGSIZE) {
		vm_page_t *page = vm_pagealloc(flags & kVMemSleep, &vm_pgkmemq);
		pmap_enter_kern(kmap.pmap, page->paddr, (vaddr_t)*out + i,
		    kVMAll);
	}

	return 0;
}

static void
internal_freewired(vmem_t *vmem, vmem_addr_t addr, vmem_size_t size)
{
	int r;

	kassert(vmem == &kmap.vmem);

	r = vmem_xfree(vmem, addr, size);
	if (r < 0) {
		kprintf("internal_freewired: vmem returned %d\n", r);
		return;
	}
	r = size;

	for (int i = 0; i < r; i += PGSIZE) {
		vm_page_t *page;
		page = pmap_unenter_kern(&kmap, (vaddr_t)addr + i);
		vm_page_free(page);
	}
}

void
vm_kernel_dump()
{
	vmem_dump(&kmap.vmem);
	vmem_dump(&vm_kernel_wired);
}

void
vm_kernel_init()
{
	vmem_earlyinit();
	vmem_init(&kmap.vmem, "kernel-va", KHEAP_BASE, KHEAP_SIZE, PGSIZE, NULL,
	    NULL, NULL, 0, kVMemBootstrap, 0);
	vmem_init(&vm_kernel_wired, "kernel-wired", 0, 0, PGSIZE,
	    internal_allocwired, internal_freewired, &kmap.vmem, 0,
	    kVMemBootstrap, 0);

	kmap.vmem.flags = 0;
	vm_kernel_wired.flags = 0;
}

vaddr_t
vm_kalloc(size_t npages, enum vm_kalloc_flags wait)
{
	vmem_addr_t addr;
	int	    flags;
	int	    r;

	flags = wait & 0x1 ? kVMemSleep : kVMemNoSleep;
	flags |= wait & 0x2 ? kVMemBootstrap : 0;
	r = vmem_xalloc(&vm_kernel_wired, npages * PGSIZE, 0, 0, 0, 0, 0, flags,
	    &addr);
	if (r == 0)
		return (vaddr_t)addr;
	else
		return (vaddr_t)NULL;
}

void
vm_kfree(vaddr_t addr, size_t npages)
{
	vmem_xfree(&vm_kernel_wired, (vmem_addr_t)addr, npages * PGSIZE);
}
