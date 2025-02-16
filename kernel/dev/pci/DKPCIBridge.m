/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Wed Sep 13 2023.
 */

#include <ddk/reg/pci.h>
#include <kdk/kmem.h>

#include "ddk/DKAxis.h"
#include "dev/pci/DKPCIBridge.h"

@implementation DKPCIBridge

@synthesize segment = m_segment;
@synthesize bus = m_bus;
@synthesize promNode = m_promNode;

- (DKPCIBridge *)parentBridge
{
	kfatal("Implement me\n");
}

- (instancetype)initWithSegment:(uint16_t)segment
			    bus:(uint8_t)bus
		       promNode:(DKDevice<DKPROMNode> *)promNode
{
	if ((self = [super init])) {
		kmem_asprintf(&m_name, "pciBridge%u:%u", segment, bus);
		m_segment = segment;
		m_bus = bus;
		m_promNode = promNode;
	}
	return self;
}

- (void)start
{
	for (unsigned slot = 0; slot < 32; slot++) {
		size_t nFun = 1;
		uint8_t headerType;

		if (pci_readw(m_segment, m_bus, slot, 0, kVendorID) == 0xffff)
			continue;

		if (pci_readb(m_segment, m_bus, slot, 0, kHeaderType) &
		    (1 << 7))
			nFun = 8;

		for (unsigned fun = 0; fun < nFun; fun++) {
			DKPCIDevice *dev;
			struct DKPCIAddress addr = {
				.segment = m_segment,
				.bus = m_bus,
				.slot = slot,
				.function = fun
			};

			headerType = pci_readb(m_segment, m_bus, slot, fun,
			    kHeaderType);

			if (headerType == 0xff)
				continue;

			dev = [[DKPCIDevice alloc] initWithBridge:self
							  address:&addr];
			[self attachChild:dev onAxis:gDeviceAxis];
			[dev addToStartQueue];
		}
	}
}

@end

@implementation DKPCI2PCIBridge

@synthesize pciDevice = m_pciDevice;

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
			      bus:(uint8_t)bus
			 promNode:(DKDevice<DKPROMNode> *)promNode;
{
	if ((self = [super initWithSegment:pciDevice.address.segment
				       bus:bus
				  promNode:promNode])) {
		m_pciDevice = pciDevice;
	}
	return self;
}

@end

@implementation DKPCIRootBridge

- (DKPCIBridge *)parentBridge
{
	return nil;
}

@end
