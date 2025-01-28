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
#include <uacpi/uacpi.h>

#include "dev/acpi/DKACPINode.h"

@interface DKACPIPlatform : DKACPINode {
}

@end

#endif /* KRX_ACPI_DKACPIPLATFORM_H */
