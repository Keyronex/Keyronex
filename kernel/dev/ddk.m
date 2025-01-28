/*
 * Copyright (c) 2023-25 NetaScale Object Solutions.
 * Created on Wed Sep 20 2023.
 */

#include <ddk/DKAxis.h>
#include <kdk/kern.h>
#include <limine.h>

#include "dev/acpi/DKACPIPlatform.h"

extern struct limine_rsdp_request rsdp_request;
DKDevice *gPlatformDevice;

void
ddk_init(void)
{
	extern void (*init_array_start)(void), (*init_array_end)(void);

	kprintf("ddk_init: DDK version 4\n");

	for (void (**func)(void) = &init_array_start; func != &init_array_end;
	     func++)
		(*func)();

	if (rsdp_request.response != NULL) {
		kprintf("ddk_init: probing ACPI platform\n");
		gPlatformDevice = [[DKACPIPlatform alloc] init];
	} else {
		kfatal("ddk_init: no platform class usable\n");
	}
}

void
ddk_autoconf(void)
{
	[gPlatformDevice start];
	[DKDevice drainStartQueue];
	[gDeviceAxis printSubtreeOfDevice:gPlatformDevice];
}
