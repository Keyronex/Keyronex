/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Wed Apr 17 2024.
 */

#include <kdk/kern.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "dev/acpi/DKACPIPlatform.h"

@implementation DKACPIPlatform

- (instancetype)init
{
	int r;

	self = [super init];

	r = uacpi_initialize(0);
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_namespace_load();
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_namespace_initialize();
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
	kassert(r == UACPI_STATUS_OK);
}

@end
