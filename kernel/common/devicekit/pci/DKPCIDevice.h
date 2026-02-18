/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2024-26 Cloudarox Solutions.
 */
/*!
 * @file DKPCIDevice.h
 * @brief PCI device.
 */

#ifndef ECX_DEVICEKIT_DKPCIDEVICE_H
#define ECX_DEVICEKIT_DKPCIDEVICE_H

#include <sys/k_intr.h>

#if defined (__OBJC__)
#include <devicekit/DKDevice.h>

@class DKPCIBridge;
@class DKPCIDevice;
#endif

enum {
	kVendorID = 0x0, /* u16 */
	kDeviceID = 0x2, /* u16 */
	kCommand = 0x4,	 /* u16 */
	kStatus = 0x6,	 /* bit 4 = capabilities list exists */
	kSubclass = 0xa,
	kClass = 0xb,
	kProgIF = 0x9,
	kHeaderType = 0xe, /* bit 7 = is multifunction */
	kBAR0 = 0x10,
	kSecondaryBus = 0x19,
	kSubordinateBus = 0x1a,
	kCapabilitiesPointer = 0x34,
	kInterruptPin = 0x3d, /* u8 */
};

typedef struct DKPCIAddress {
	uint16_t segment;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
} DKPCIAddress;

typedef struct DKPCIMatchData {
	uint16_t vendor;
	uint16_t device;
	uint8_t class;
	uint8_t subclass;
	uint8_t prog_if;
} DKPCIMatchData;

typedef struct DKPCIBarInfo {
	enum {
		kPCIBarMem,
		kPCIBarIO,
	} type;
	uint64_t base;
	uint64_t size;
} DKPCIBarInfo;

#if defined (__OBJC__)
@protocol DKPCIDeviceMatching

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice;

@end

@interface DKPCIDevice: DKDevice {
	DKPCIBridge *m_bridge;
	DKPCIAddress m_address;

	/* MSI-X state */
	vaddr_t m_msixTable;
	uint16_t m_msixCap;
	uint16_t m_lastAllocatedMSIxVector; /* bump allocation for now */

	/* MSI state */
	uint16_t m_msiCap;
	uint16_t m_msiCount;
}

@property (readonly) DKPCIAddress address;
@property (readonly) DKPCIBridge *bridge;

+ (void)registerMatchingClass:(Class<DKPCIDeviceMatching>)matchingClass;

- (instancetype)initWithBridge:(DKPCIBridge *)bridge
		       address:(DKPCIAddress *)address;



- (DKPCIBarInfo)barInfo:(uint8_t)bar;

- (uint8_t)configRead8:(uint16_t)offset;
- (uint16_t)configRead16:(uint16_t)offset;
- (uint32_t)configRead32:(uint16_t)offset;
- (void)configWrite8:(uint16_t)offset value:(uint8_t)value;
- (void)configWrite16:(uint16_t)offset value:(uint16_t)value;
- (void)configWrite32:(uint16_t)offset value:(uint32_t)value;

- (uint16_t)findCapabilityByID:(uint8_t)id;
- (uint16_t)findCapabilityByID:(uint8_t)id startingOffset:(uint16_t)offset;

- (void)setCommandFlag:(uint16_t)flag enabled:(bool)enabled;
- (void)setMemorySpace:(bool)enabled;
- (void)setBusMastering:(bool)enabled;
- (void)setInterrupts:(bool)enabled;

- (kirq_source_t)intxIrqSource;

@end
#endif /* defined(__OBJC__) */

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

#endif /* ECX_DEVICEKIT_DKPCIDEVICE_H */
