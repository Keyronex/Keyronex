/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024-26 Cloudarox Solutions.
 */
/*!
 * @file DKPCIBridge.h
 * @brief PCI bridge.
 */

#ifndef ECX_PCI_DKPCIBRIDGE_H
#define ECX_PCI_DKPCIBRIDGE_H

#include <devicekit/pci/DKPCIDevice.h>

@class DKPROMNode;

@interface DKPCIBridge : DKDevice {
	DKPROMNode *m_promNode;
	uint16_t m_segment;
	uint8_t m_bus;
}

@property (readonly) DKPROMNode*promNode;
@property (readonly) DKPCIBridge *parentBridge;
@property (readonly) uint16_t segment;
@property (readonly) uint8_t bus;

- (instancetype)initWithSegment:(uint16_t)segment
			    bus:(uint8_t)bus
		       promNode:(DKPROMNode *)promNode;

- (void)start;

@end

@interface DKPCI2PCIBridge : DKPCIBridge {
	DKPCIDevice *m_pciDevice;
}

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
			      bus:(uint8_t)bus
			 promNode:(DKPROMNode *)promNode;

@property (readonly) DKPCIDevice *pciDevice;

@end

@interface DKPCIRootBridge : DKPCIBridge

@end


#endif /* ECX_PCI_DKPCIBRIDGE_H */
