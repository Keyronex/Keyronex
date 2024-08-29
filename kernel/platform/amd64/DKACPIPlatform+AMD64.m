/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#include "IOAPIC.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/tables.h"
#include "kdk/libkern.h"
#include "uacpi/tables.h"
#include "uacpi/uacpi.h"

@implementation DKACPIPlatform (AMD64_)

static void
parse_ioapics(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_ioapic *ioapic;

	if (item->type != 1)
		return;

	ioapic = (struct acpi_madt_ioapic *)item;
	[[IOApic alloc] initWithProvider:arg
				      id:ioapic->id
				 address:(paddr_t)ioapic->address
				 gsiBase:ioapic->gsi_base];
}

static void
parse_isa_overrides(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_interrupt_source_override *intr;
	struct isa_intr_override *override;

	if (item->type != 2)
		return;

	intr = (struct acpi_madt_interrupt_source_override *)item;
	override = &isa_intr_overrides[intr->source];

	override->gsi = intr->gsi;
	override->lopol = (intr->flags & 0x2) == 0x2 ? 0x1 : 0x0;
	override->edge = (intr->flags & 0x8) == 0x8 ? 0x1 : 0x0;
}

- (void)iterateArchSpecificEarlyTables
{
	uacpi_table madt;
	int r;

	r = uacpi_table_find_by_signature("APIC", &madt);
	if (r != UACPI_STATUS_OK)
		kfatal("No MADT table - cannot boot.\n");

	for (int i = 0; i < 16; i++) {
		isa_intr_overrides[i].gsi = i;
		isa_intr_overrides[i].lopol = false;
		isa_intr_overrides[i].edge = false;
	}

	dk_acpi_madt_walk((struct acpi_madt *)madt.virt_addr, parse_ioapics,
	    self);
	dk_acpi_madt_walk((struct acpi_madt *)madt.virt_addr,
	    parse_isa_overrides, self);
}

@end
