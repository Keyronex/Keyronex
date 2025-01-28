/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Wed Sep 13 2023.
 */

#ifndef KRX_PCI_DKPCIBRIDGE_H
#define KRX_PCI_DKPCIBRIDGE_H

#include <ddk/DKPCIDevice.h>

@interface DKPCIBridge : DKDevice {
	uint16_t m_segment;
	uint8_t m_bus;
}

@end

@interface DKPCIRootBridge : DKPCIBridge {
}

- (instancetype)initWithSegment:(uint16_t)segment bus:(uint8_t)bus;

- (void)start;

@end

#endif /* KRX_PCI_DKPCIBRIDGE_H */
