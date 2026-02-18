/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file Virt68kPlatform.m
 * @brief Platform root device for qemu m68k 'virt'
 */

#include <sys/k_log.h>
#include <sys/kmem.h>

#include <devicekit/virtio/VirtIOMMIOTransport.h>
#include <devicekit/DKAxis.h>
#include <devicekit/DKDevice.h>
#include <devicekit/DKPlatformRoot.h>

void DKLogAttach(DKDevice *child, DKDevice *parent);

@interface VirtM68KPlatform : DKDevice <DKPlatformRoot>

@end

@implementation VirtM68KPlatform

- (instancetype)init
{
	self = [super init];

	kmem_asprintf(&m_name, "virt68k-platform");

	DKLogAttach(self, nil);

	return self;
}

- (void)start
{
	volatile uint8_t *virtio_base = (void *)0xff010000;

	for (int i = 0; i < 128; i++) {
		id dev = [DKVirtIOMMIOTransport
		    probeWithMMIO:virtio_base + 0x200 * i
			interrupt:32 + i];
		if (dev != nil) {
			[self attachChild:dev onAxis:gDeviceAxis];
			[dev addToStartQueue];
		}
	}
}

- (void)routePCIPin:(uint8_t)pin
	  forBridge:(DKPCIBridge *)bridge
	       slot:(uint8_t)slot
	   function:(uint8_t)fun
	       into:(out kirq_source_t *)source
{
	kfatal("No PCI");
}

- (int)handleSource:(struct kirq_source *)source
	withHandler:(kirq_handler_t *)handler
	   argument:(void *)arg
	 atPriority:(ipl_t *)ipl
	  irqObject:(out kirq_t *)object
{
	kfatal("Implement me\n");
}

@end

void
dk_platform_threaded_init(void)
{
	gPlatformRoot = [[VirtM68KPlatform alloc] init];
	[gPlatformRoot start];
	[DKDevice drainStartQueue];
}
