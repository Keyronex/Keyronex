/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Wed Sep 13 2025.
 */

#ifndef KRX_DDK_DKPCIDEVICE_H
#define KRX_DDK_DKPCIDEVICE_H

#include <ddk/DKDevice.h>

@class DKPCIBridge;

typedef struct DKPCIAddress {
	uint16_t segment;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
} DKPCIAddress;

typedef struct DKPCIMatchData {
	uint16_t vendor;
	uint16_t device;
	uint16_t class;
	uint16_t subclass;
} DKPCIMatchData;

typedef struct DKPCIBarInfo {
	enum {
		kPCIBarMem,
		kPCIBarIO,
	} type;
	uint64_t base;
	uint64_t size;
} DKPCIBarInfo;

@protocol DKPCIDeviceMatching

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

@end

@interface DKPCIDevice : DKDevice {
	DKPCIBridge *m_bridge;
	DKPCIAddress m_address;
}

+ (void)registerMatchingClass:(Class<DKPCIDeviceMatching>)matchingClass;

- (instancetype)initWithBridge:(DKPCIBridge *)bridge
		       address:(DKPCIAddress *)address;

@end

uint8_t pci_readb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset);
uint16_t pci_readw(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset);
uint32_t pci_readl(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset);
void pci_writeb(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint8_t value);
void pci_writew(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint16_t value);
void pci_writel(uint16_t seg, uint32_t bus, uint32_t slot, uint32_t function,
    uint32_t offset, uint32_t value);

#endif /* KRX_DDK_DKPCIDEVICE_H */
