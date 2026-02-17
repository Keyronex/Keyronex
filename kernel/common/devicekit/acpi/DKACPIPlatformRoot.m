/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file DKACPIPlatformRoot.m
 * @brief ACPI platform root device implementation.
 */

#include <keyronex/dlog.h>

#include <devicekit/DKAxis.h>
#include <devicekit/acpi/DKACPINode.h>
#include <devicekit/acpi/DKACPIPlatformRoot.h>

#include <uacpi/acpi.h>
#include <uacpi/tables.h>
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

+ (instancetype)root
{
	return gPlatformRoot;
}

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

- (void)handleMADTEntry:(struct acpi_entry_hdr *)item
{
	kfatal("subclass responsibility");
}

- (void)iterateMADT
{
	struct acpi_entry_hdr *entry;
	struct acpi_madt *madt;
	uint8_t *madt_lim;
	uacpi_table madt_table;
	int r;

	r = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt_table);
	if (r != UACPI_STATUS_OK)
		kfatal("Failed to find MADT: %d\n", r);

	madt = madt_table.ptr;
	madt_lim = ((uint8_t *)madt) + madt->hdr.length;

	for (uint8_t *elem = (uint8_t *)madt->entries; elem < madt_lim;
	    elem += entry->length) {
		entry = (struct acpi_entry_hdr *)elem;
		[self handleMADTEntry:entry];
	}

	uacpi_table_unref(&madt_table);
}

- (void)start
{
	[super start];
	[DKDevice drainStartQueue];
	[self iterateMADT];
	[self startDevices];
	[DKACPINode drainStartDevicesQueue];
	[DKDevice drainStartQueue];
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
	[gDeviceAxis printSubtreeOfDevice:gPlatformRoot];
}
