/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Wed Apr 17 2024.
 */
/*!
 * @file DKACPINode.h
 * @brief Declares the ACPI platform root device class.
 */

#ifndef KRX_ACPI_DKACPIPLATFORM_H
#define KRX_ACPI_DKACPIPLATFORM_H

#include <ddk/DKDevice.h>
#include <ddk/DKPlatformRoot.h>

#include "dev/acpi/DKACPINode.h"

struct acpi_madt;
struct acpi_entry_hdr;

@interface DKACPIPlatform : DKACPINode <DKPlatformRoot>

+ (instancetype)root;

@end

void dk_acpi_madt_walk(struct acpi_madt *madt,
    void (*callback)(struct acpi_entry_hdr *item, void *arg), void *arg);

#endif /* KRX_ACPI_DKACPIPLATFORM_H */
