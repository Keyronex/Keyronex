/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#include "GICv2Distributor.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/tables.h"
#include "kdk/executive.h"
#include "kdk/kern.h"
#include "kdk/libkern.h"
#include "uacpi/tables.h"
#include "uacpi/uacpi.h"

@implementation DKACPIPlatform (AArch64)

static paddr_t last_gicc_base = 0;

static void
parse_giccs(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_gicc *pgicc, gicc;
	bool matched = false;

	if (item->type != 0xb)
		return;

	memcpy(&gicc, item, sizeof(struct acpi_madt_gicc));
	pgicc = (void *)&gicc;

	kprintf("Found a GICC:"
		" GIC interface num %u, ACPI UID %u, MPIDR %lu,"
		" base address 0x%zx\n",
	    pgicc->interface_number, pgicc->acpi_id, pgicc->mpidr,
	    pgicc->address);

	if (last_gicc_base != 0)
		kassert(last_gicc_base == pgicc->address);

	for (size_t i = 0; i < ncpus; i++) {
		if (cpus[i]->cpucb.mpidr == pgicc->mpidr) {
			int r;

			kassert(!matched);
			matched = true;

			cpus[i]->cpucb.gic_interface_number =
			    pgicc->interface_number;

			r = vm_ps_map_physical_view(kernel_process->vm,
			    &cpus[i]->cpucb.gicc_base, PGSIZE, pgicc->address,
			    kVMAll, kVMAll, false);
			kassert(r == 0);
		}
	}

	kassert(matched);
}

static void
parse_gicds(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_gicd *pgicd, gicd;
	extern vaddr_t gicd_base;
	int r;

	if (item->type != 0xc)
		return;

	memcpy(&gicd, item, sizeof(struct acpi_madt_gicd));
	pgicd = (void *)&gicd;

	kprintf("Found a GICD: GIC version num %d, base address 0x%zx\n",
	    pgicd->gic_version, pgicd->address);

	r = vm_ps_map_physical_view(kernel_process->vm, &gicd_base, PGSIZE,
	    pgicd->address, kVMAll, kVMAll, false);
	kassert(r == 0);
}

static void
gtdt_walk(void)
{
	uacpi_table table;
	acpi_table_gtdt_t *gtdt;
	int r;

	r = uacpi_table_find_by_signature("GTDT", &table);
	if (r != 0)
		kfatal("No GTDT table found!\n");

	gtdt = (void *)table.virt_addr;

	kprintf("GTDT: %p/ EL1 NS: GSIV %u, Flags 0x%x\n", gtdt,
	    gtdt->nonsecure_el1_interrupt, gtdt->nonsecure_el1_flags);
}

- (void)iterateArchSpecificEarlyTables
{
	uacpi_table madt;
	int r;

	r = uacpi_table_find_by_signature("APIC", &madt);
	if (r != UACPI_STATUS_OK)
		kfatal("No MADT table - cannot boot.\n");

	dk_acpi_madt_walk((struct acpi_madt *)madt.virt_addr, parse_giccs,
	    NULL);
	dk_acpi_madt_walk((struct acpi_madt *)madt.virt_addr, parse_gicds,
	    NULL);
	gtdt_walk();
}

@end
