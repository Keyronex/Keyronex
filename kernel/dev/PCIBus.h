#ifndef PCIBUS_H_
#define PCIBUS_H_

#include <md/intr.h>
#include <vm/vm.h>

#include <devicekit/DKDevice.h>
#include <lai/core.h>

@class PCIBus;

typedef struct dk_device_pci_info {
	PCIBus	*busObj; /* PCIBus * */
	uint16_t seg;
	uint8_t	 bus;
	uint8_t	 slot;
	uint8_t	 fun;

	uint8_t pin;
} dk_device_pci_info_t;

typedef struct dk_pci_cap {
	uint8_t cap_vndr; /* vendor */
	uint8_t cap_next; /* next cap */
	uint8_t cap_len;  /* cap length */
			  /* maybe more...*/
} dk_pci_cap_t;

#define PCIINFO_CFG_READ(WIDTH, PCIINFO, OFFSET)                           \
	laihost_pci_read##WIDTH(pciInfo->seg, pciInfo->bus, pciInfo->slot, \
	    pciInfo->fun, OFFSET)

@interface PCIBus : DKDevice

/** handle INTx */
+ (int)handleInterruptOf:(dk_device_pci_info_t *)pciInfo
	     withHandler:(intr_handler_fn_t)handler
		argument:(void *)arg
	      atPriority:(ipl_t)priority;
+ (void)enableMemorySpace:(dk_device_pci_info_t *)pciInfo;
+ (void)enableBusMastering:(dk_device_pci_info_t *)pciInfo;
+ (void)setInterruptsOf:(dk_device_pci_info_t *)pciInfo enabled:(BOOL)enabled;
+ (void)enumerateCapabilitiesOf:(dk_device_pci_info_t *)pciInfo
		   withCallback:(void (*)(dk_device_pci_info_t *, voff_t pCap,
				    void *))callback
				    userData:(void*)userData;
+ (paddr_t)getBar:(uint8_t)num info:(dk_device_pci_info_t *)pciInfo;

+ (BOOL)probeWithAcpiNode:(lai_nsnode_t *)node provider:(DKDevice *)provider;
;

- initWithSeg:(uint8_t)seg bus:(uint8_t)bus provider:(DKDevice *)provider;
;

@end

#endif /* PCIBUS_H_ */
