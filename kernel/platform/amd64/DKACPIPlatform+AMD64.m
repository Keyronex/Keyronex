/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#include <kdk/amd64.h>
#include <kdk/amd64/regs.h>
#include <kdk/kern.h>

#include "dev/acpi/DKACPIPlatform.h"

DKDevice<DKPlatformRoot> *gPlatformRoot;

@implementation DKACPIPlatform (AMD64_)

- (int)allocateLeastLoadedMSIxInterruptForEntry:(struct intr_entry *)entry
				    msixAddress:(out uint32_t *)msixAddress
				       msixData:(out uint32_t *)msixData
{
	uint8_t vector;
	int r = md_intr_alloc("MSI-X", kIPLDevice, entry->handler, entry->arg,
	    false, &vector, entry);
	if (r != 0)
		return r;

	*msixAddress = rdmsr(kAMD64MSRAPICBase) & 0xfffff000;
	*msixData = (cpus[0]->cpucb.lapic_id << 24) | vector;

	return 0;
}

#if 0
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
#endif

@end
