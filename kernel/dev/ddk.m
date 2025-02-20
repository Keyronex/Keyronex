/*
 * Copyright (c) 2023-25 NetaScale Object Solutions.
 * Created on Wed Sep 20 2023.
 */

#include <ddk/DKAxis.h>
#include <kdk/kern.h>
#include <kdk/vm.h>
#include <limine.h>

#include "dev/acpi/DKACPIPlatform.h"
#include "vm/vmp.h"

extern struct limine_rsdp_request rsdp_request;
extern struct limine_dtb_request dtb_request;
Class gPlatformSpecificRootClass;
DKDevice *gPlatformDevice;

int
dk_allocate_and_map(vaddr_t *out_vaddr, paddr_t *out_paddr, size_t size)
{
	vm_page_t *page;
	int r;

	r = vm_page_alloc(&page, vm_bytes_to_order(size), kPageUseKWired,
	    false);
	if (r != 0)
		return r;

	r = vm_ps_map_physical_view(&kernel_procstate, out_vaddr,
	    PGROUNDUP(size), vm_page_paddr(page), kVMRead | kVMWrite,
	    kVMRead | kVMWrite, false);
	if (r != 0) {
		vm_page_delete(page);
		vm_page_release(page);
		return r;
	}
	*out_paddr = vm_page_paddr(page);
	return 0;
}

void
ddk_init(void)
{
	extern void (*init_array_start)(void), (*init_array_end)(void);

	kprintf("ddk_init: DDK version 4\n");

	for (void (**func)(void) = &init_array_start; func != &init_array_end;
	     func++)
		(*func)();

#if !defined(__m68k__)
	if (rsdp_request.response != NULL) {
		kprintf("ddk_init: probing ACPI platform\n");
		gPlatformDevice = [[DKACPIPlatform alloc] init];
	} else
#endif
	if (dtb_request.response != NULL) {
		kfatal("ddk_init: no support for openfirmware yet\n");
	} else if (gPlatformSpecificRootClass != Nil) {
		kprintf("ddk_init: probing platform-specific root\n");
		gPlatformDevice = [[gPlatformSpecificRootClass alloc] init];
	}
}

void
ddk_autoconf(void)
{
	ktimer_t timer;

	[gPlatformDevice start];
	[DKDevice drainStartQueue];

	ke_timer_init(&timer);
	ke_timer_set(&timer, NS_PER_S * 2);
	kprintf("ddk_autoconf: waiting for devices to settle...\n");
	ke_wait(&timer, "wait for timer", 0,0,-1);

#if 1
	[gDeviceAxis printSubtreeOfDevice:gPlatformDevice];
#endif
}
