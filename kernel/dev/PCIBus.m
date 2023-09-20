#include "PCIBus.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "lai/helpers/pci.h"
#include "lai/host.h"
#include "ntcompat/NTStorPort.h"

enum {
	kVendorID = 0x0, /* u16 */
	kDeviceID = 0x2, /* u16 */
	kCommand = 0x4,	 /* u16 */
	kStatus = 0x6,	 /* bit 4 = capabilities list exists */
	kSubclass = 0xa,
	kClass = 0xb,
	kHeaderType = 0xe, /* bit 7 = is multifunction */
	kBaseAddress0 = 0x10,
	kCapabilitiesPointer = 0x34,
	kInterruptPin = 0x3d, /* u8 */
};

enum {
	kCapMSIx = 0x11,
};

@implementation PCIBus

- (void)doSegment:(uint16_t)seg
	      bus:(uint8_t)bus
	     slot:(uint8_t)slot
	 function:(uint8_t)fun
{
	struct pci_dev_info info;

#define CFG_READ(WIDTH, OFFSET) \
	laihost_pci_read##WIDTH(seg, bus, slot, fun, OFFSET)

	info.vendorId = CFG_READ(w, kVendorID);
	if (info.vendorId == 0xffff)
		return;
	info.deviceId = CFG_READ(w, kDeviceID);
	info.klass = CFG_READ(b, kClass);
	info.subClass = CFG_READ(b, kSubclass);

	info.seg = seg;
	info.bus = bus;
	info.slot = slot;
	info.fun = fun;
	info.pin = CFG_READ(b, kInterruptPin);

	if (info.pin != 0) {
		int r;
		acpi_resource_t res;

		r = lai_pci_route_pin(&res, info.seg, info.bus, info.slot, info.fun, info.pin);
		if (r != LAI_ERROR_NONE) {
			kfatal("failed to route pin!\n");
		} else {
			info.gsi = res.base;
			info.lopol = res.irq_flags & ACPI_SMALL_IRQ_ACTIVE_LOW;
			info.edge = res.irq_flags &
			    ACPI_SMALL_IRQ_EDGE_TRIGGERED;
		}
	}

#if 0
	kprintf("%x:%x (klass %x subclass %x)\n", info.vendorId, info.deviceId,
	    info.klass, info.subClass);
#endif

	[NTStorPort probeWithPCIBus:self info:&info];

}

- (instancetype)initWithProvider:(DKDevice *)provider
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus

{
	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "pcibus-%d-%d", seg, bus);

	[self registerDevice];
	DKLogAttach(self);

	for (unsigned slot = 0; slot < 32; slot++) {
		size_t nFun = 1;

		if (laihost_pci_readw(seg, bus, slot, 0, kVendorID) == 0xffff)
			continue;

		if (laihost_pci_readb(seg, bus, slot, 0, kHeaderType) &
		    (1 << 7))
			nFun = 8;

		for (unsigned fun = 0; fun < nFun; fun++)
			[self doSegment:seg bus:bus slot:slot function:fun];
	}

	return self;
}

@end
