/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file DKACPIPlatformRoot.m
 * @brief ACPI platform root device implementation.
 */

#include <devicekit/acpi/DKACPIPlatformRoot.h>
#include <keyronex/dlog.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

@implementation DKACPIPlatform

@end

void
dk_acpi_early_init(void)
{
	kassert(uacpi_initialize(0) == UACPI_STATUS_OK);
	kassert(uacpi_namespace_load() == UACPI_STATUS_OK);
	kassert(uacpi_namespace_initialize() == UACPI_STATUS_OK);
	kassert(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC) ==
	    UACPI_STATUS_OK);
}
