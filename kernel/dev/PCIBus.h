#ifndef KRX_DEV_PCIBUS_H
#define KRX_DEV_PCIBUS_H

#include "ddk/DKDevice.h"

#if defined(__arch64__) || defined (__amd64__)
#include "uacpi/types.h"
#include "uacpi/uacpi.h"
#endif

struct pci_dev_info {
	uint16_t seg;
	uint8_t bus;
	uint8_t slot;
	uint8_t fun;

	uint8_t klass, subClass;
	uint16_t vendorId, deviceId;

	/* intx pin */
	uint8_t pin;
	/*! global system interrupt number (0 for none) */
	uint8_t gsi;
	/* low polarity? */
	bool lopol;
	/* edge triggered? */
	bool edge;
};

#if defined(__arch64__) || defined (__amd64__)
@protocol DKPCIDeviceDelegate

- (void)capabilityEnumeratedAtOffset:(voff_t)capOffset;

@end

@interface PCIBus : DKDevice {
	uacpi_namespace_node *acpiNode;
}

+ (void)enableBusMasteringForInfo:(struct pci_dev_info *)info;
+ (void)setMemorySpaceForInfo:(struct pci_dev_info *)info enabled:(BOOL)enabled;
+ (void)enumerateCapabilitiesForInfo:(struct pci_dev_info *)info
			    delegate:(DKDevice<DKPCIDeviceDelegate> *)delegate;
+ (paddr_t)getBar:(int)i forInfo:(struct pci_dev_info *)info;
+ (void)setInterruptsEnabled:(BOOL)enabled forInfo:(struct pci_dev_info *)info;

- (instancetype)initWithProvider:(DKDevice *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus;

@end
#else
@class PCIBus;
#endif

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

#endif /* KRX_DEV_PCIBUS_H */
