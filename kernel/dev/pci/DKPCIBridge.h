/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Wed Sep 13 2023.
 */

#ifndef KRX_PCI_DKPCIBRIDGE_H
#define KRX_PCI_DKPCIBRIDGE_H

#include <ddk/DKPCIDevice.h>

@protocol DKPROMNode;

@interface DKPCIBridge : DKDevice {
	DKDevice<DKPROMNode> *m_promNode;
	uint16_t m_segment;
	uint8_t m_bus;
}

@property (readonly) DKDevice<DKPROMNode> *promNode;
@property (readonly) DKPCIBridge *parentBridge;
@property (readonly) uint16_t segment;
@property (readonly) uint8_t bus;

- (instancetype)initWithSegment:(uint16_t)segment
			    bus:(uint8_t)bus
		       promNode:(DKDevice<DKPROMNode> *)promNode;

- (void)start;

@end

@interface DKPCI2PCIBridge : DKPCIBridge {
	DKPCIDevice *m_pciDevice;
}

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
			      bus:(uint8_t)bus
			 promNode:(DKDevice<DKPROMNode> *)promNode;

@property (readonly) DKPCIDevice *pciDevice;

@end

@interface DKPCIRootBridge : DKPCIBridge

@end

#endif /* KRX_PCI_DKPCIBRIDGE_H */
