/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Wed Sep 13 2025.
 */

#ifndef KRX_DDK_DKPCIDEVICE_H
#define KRX_DDK_DKPCIDEVICE_H

#include <ddk/DKDevice.h>
#include <kdk/vmtypes.h>

@class DKPCIBridge;
@class DKPCIDevice;
struct intr_entry;

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

@protocol DKPCIDeviceMatching

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice;

@end

@interface DKPCIDevice : DKDevice {
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

- (uint16_t)availableMSIxVectors;
- (int)setMSIx:(bool)enabled;
- (int)allocateLeastLoadedMSIxInterruptForEntry:(struct intr_entry *)entry;

- (uint8_t)availableMSIVectors;
- (int)allocateLeastLoadedMSIInterruptsForEntries:(struct intr_entry *)entries
					    count:(uint8_t)count;

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
