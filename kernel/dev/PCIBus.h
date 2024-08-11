/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created in 2023.
 */

#ifndef KRX_DEV_PCIBUS_H
#define KRX_DEV_PCIBUS_H

#include "ddk/DKDevice.h"
#include "dev/pci_reg.h"

struct pci_dev_info {
	uint16_t seg;
	uint8_t bus;
	uint8_t slot;
	uint8_t fun;

	uint8_t klass, subClass;
	uint16_t vendorId, deviceId;

	uint8_t pin;
	dk_interrupt_source_t intx_source;
};

@protocol DKPCIDeviceDelegate

- (void)capabilityEnumeratedAtOffset:(voff_t)capOffset;

@end

/*!
 * Abstract class representing a PCI bus.
 */
@interface PCIBus : DKDevice {
	uint16_t m_seg;
	uint8_t m_bus;
}

+ (void)setECAMBase:(paddr_t)base;
+ (paddr_t)getECAMBaseForSegment:(uint16_t)seg bus:(uint8_t)bus;

+ (void)enableBusMasteringForInfo:(struct pci_dev_info *)info;
+ (void)setMemorySpaceForInfo:(struct pci_dev_info *)info enabled:(BOOL)enabled;
+ (void)setInterruptsEnabled:(BOOL)enabled forInfo:(struct pci_dev_info *)info;
+ (void)enumerateCapabilitiesForInfo:(struct pci_dev_info *)info
			    delegate:(DKDevice<DKPCIDeviceDelegate> *)delegate;
+ (paddr_t)getBar:(int)i forInfo:(struct pci_dev_info *)info;


/*!
 * Called by subclasses only.
 */
- (void)enumerateDevices;

/*!
 * Implemented by subclasses.
 */
- (dk_interrupt_source_t)routePCIPinForInfo:(struct pci_dev_info *)info;

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

#define PCIINFO_CFG_READ(WIDTH, PCIINFO, OFFSET)                         \
	pci_read##WIDTH((PCIINFO)->seg, (PCIINFO)->bus, (PCIINFO)->slot, \
	    (PCIINFO)->fun, OFFSET)

#define PCIINFO_CFG_WRITE(WIDTH, PCIINFO, OFFSET, VALUE)                  \
	pci_write##WIDTH((PCIINFO)->seg, (PCIINFO)->bus, (PCIINFO)->slot, \
	    (PCIINFO)->fun, OFFSET, VALUE)

#endif /* KRX_DEV_PCIBUS_H */
