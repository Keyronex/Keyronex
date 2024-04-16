#ifndef KRX_DEV_PCIBUS_H
#define KRX_DEV_PCIBUS_H

#include "ddk/DKDevice.h"
#include "uacpi/types.h"
#include "uacpi/uacpi.h"

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

@interface PCIBus : DKDevice {
	uacpi_namespace_node *acpiNode;
}

- (instancetype)initWithProvider:(DKDevice *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus;

@end

#endif /* KRX_DEV_PCIBUS_H */
