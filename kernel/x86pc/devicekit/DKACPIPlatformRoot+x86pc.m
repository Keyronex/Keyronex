/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file DKACPIPlatformRoot+x86pc.m
 * @brief DKACPIPlatformRoot category for x86pc.
 */

#include <keyronex/dlog.h>

#include <devicekit/acpi/DKACPIPlatformRoot.h>
#include <devicekit/DKAxis.h>
#include <devicekit/IOAPIC.h>

#include <uacpi/acpi.h>

@implementation DKACPIPlatform (x86pc)

- (void)handleMADTEntry:(struct acpi_entry_hdr *)entry
{
	switch (entry->type) {
	case ACPI_MADT_ENTRY_TYPE_IOAPIC: {
		struct acpi_madt_ioapic *ioapic;
			IOApic *node;

		ioapic = (struct acpi_madt_ioapic *)entry;

		node = [[IOApic alloc] initWithId:ioapic->id
					  address:ioapic->address
					  gsiBase:ioapic->gsi_base];
		[self attachChild:node onAxis:gDeviceAxis];
		[node start];

		break;
	}

	case ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE: {
		struct acpi_madt_interrupt_source_override *intr;
		struct isa_intr_override *override;

		intr = (struct acpi_madt_interrupt_source_override *)entry;
		override = &isa_intr_overrides[intr->source];

		override->gsi = intr->gsi;
		override->lopol = (intr->flags & 0x2) == 0x2;
		override->edge = (intr->flags & 0x8) == 0x8;

		break;
	}
	}
}

@end
