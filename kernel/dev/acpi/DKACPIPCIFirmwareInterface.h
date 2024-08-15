/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#ifndef KRX_ACPI_ACPIPCIBUS_H
#define KRX_ACPI_ACPIPCIBUS_H

#include "DKAACPIPlatform.h"
#include "ddk/DKObject.h"
#include "dev/pci/DKPCIBus.h"
#include "uacpi/uacpi.h"

@interface DKACPIPCIFirmwareInterface : DKObject <DKPCIFirmwareInterfacing> {
	DKACPIPCIFirmwareInterface *m_parent;
	DKPCIBus *m_PCIBus;
	uacpi_namespace_node *acpiNode;
	uint8_t m_upstreamSlot;
	uint8_t m_upstreamFunction;
}

@property (readonly) DKPCIBus *pciBus;

- (instancetype)initWithProvider:(DKDevice *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus;

@end

#endif /* KRX_ACPI_ACPIPCIBUS_H */
