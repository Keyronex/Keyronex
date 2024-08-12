/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#ifndef KRX_ACPI_ACPIPCIBUS_H
#define KRX_ACPI_ACPIPCIBUS_H

#include "DKAACPIPlatform.h"
#include "dev/pci/DKPCIBus.h"
#include "uacpi/uacpi.h"

@interface ACPIPCIBus : DKPCIBus {
	uacpi_namespace_node *acpiNode;
}

- (instancetype)initWithProvider:(DKACPIPlatform *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus;

@end

#endif /* KRX_ACPI_ACPIPCIBUS_H */
