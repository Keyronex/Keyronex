/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#include "APLIC.h"
#include "dev/acpi/DKACPIPlatform.h"
#include "kdk/libkern.h"
#include "uacpi/tables.h"
#include "uacpi/acpi.h"
#include "uacpi/uacpi.h"

@implementation DKACPIPlatform (RISCV)

/*
 * [31:24] -> APLIC identifier.
 * [15:0] -> APLIC IDC Hart index.
 */

static uint8_t
ext_intc_id_to_aplic_id(uint32_t ext_intc_id)
{
	return ext_intc_id >> 24;
}

static uint16_t
ext_intc_id_to_aplic_idc_hartindex(uint32_t ext_intc_id)
{
	return ext_intc_id & 0xffff;
}

static void
parse_riscv(struct acpi_entry_hdr *item, void *arg)
{
	switch (item->type) {
	case 0x18: { /* RINTC */

		struct acpi_madt_rintc rintc;
		memcpy(&rintc, item, sizeof(struct acpi_madt_rintc));
		kprintf("Rintc: Hart ID %zu; APLIC id %hhu; "
			"APLIC IDC HartIndex %hu\n",
		    rintc.hart_id, ext_intc_id_to_aplic_id(rintc.ext_intc_id),
		    ext_intc_id_to_aplic_idc_hartindex(rintc.ext_intc_id));
		bootstrap_cpu.cpucb.aplic_idc_hartindex =
		    ext_intc_id_to_aplic_idc_hartindex(rintc.ext_intc_id);
		break;
	}

	case 0x1a: { /* APLIC */
		struct acpi_madt_aplic aplic;
		memcpy(&aplic, item, sizeof(struct acpi_madt_aplic));
		[[APLIC alloc] initWithProvider:arg madtAplicEntry:&aplic];
		break;
	}

	default: {

		kprintf("Found something of type 0x%x\n", item->type);
	}
	}
}

- (void)iterateArchSpecificEarlyTables
{
	uacpi_table madt;
	int r;

	r = uacpi_table_find_by_signature("APIC", &madt);
	if (r != UACPI_STATUS_OK)
		kfatal("No MADT table - cannot boot.\n");

	dk_acpi_madt_walk((struct acpi_madt *)madt.virt_addr, parse_riscv,
	    self);
}

- (DKPlatformInterruptController *)platformInterruptController
{
	return (id)[APLIC class];
}

@end
