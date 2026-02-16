/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file DKACPIPlatformRoot.m
 * @brief ACPI platform root device implementation.
 */

#include <devicekit/DKAxis.h>
#include <devicekit/acpi/DKACPINode.h>
#include <devicekit/acpi/DKACPIPlatformRoot.h>

#include <keyronex/dlog.h>

#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>


@interface DKACPINode ()
+ (void)drainStartDevicesQueue;
- (void)addToStartDevicesQueue;
- (void)startDevices;
@end

void DKLogAttach(DKDevice *child, DKDevice *parent);

DKAxis *gACPIAxis;
DKACPIPlatform *gPlatformRoot;

@implementation DKACPIPlatform

- (instancetype)init
{
	kassert(uacpi_initialize(0) == UACPI_STATUS_OK);
	kassert(uacpi_namespace_load() == UACPI_STATUS_OK);
	kassert(uacpi_namespace_initialize() == UACPI_STATUS_OK);
	kassert(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC) ==
	    UACPI_STATUS_OK);

	self = [super initWithNamespaceNode:uacpi_namespace_root()];
	if (self != nil) {
		m_nsNode = uacpi_namespace_root();

		gACPIAxis = [DKAxis axisWithName:"DKACPI"];
		[gACPIAxis addChild:self ofParent:nil];
		DKLogAttach(self, nil);
	}
	return self;
}

- (void)start
{
	[super start];
	[DKDevice drainStartQueue];
	[self startDevices];
	[DKACPINode drainStartDevicesQueue];
}

@end

void
dk_acpi_presmp_init(void)
{
	gPlatformRoot = [[DKACPIPlatform alloc] init];
}

void
dk_acpi_threaded_init(void)
{
	[gPlatformRoot start];
}
