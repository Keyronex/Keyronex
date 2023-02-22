/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */

#include "kdk/object.h"

#include "pcibus.hh"

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

PCIDevice::PCIDevice(PCIBus *provider, pci_device_info &info)
    : info(info)
{
	kmem_asprintf(&objhdr.name, "pcidev%d:%d:%d.%d", info.seg, info.bus,
	    info.slot, info.fun);
	attach(provider);
}

void
PCIBus::doFunction(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun)
{
	pci_device_info pciInfo;
	uint8_t klass, subClass;
	uint16_t vendorId, deviceId;
	PCIDevice *pcidev;

#define CFG_READ(WIDTH, OFFSET) \
	laihost_pci_read##WIDTH(seg, bus, slot, fun, OFFSET)

	vendorId = CFG_READ(w, kVendorID);
	if (vendorId == 0xffff)
		return;
	deviceId = CFG_READ(w, kDeviceID);
	klass = CFG_READ(b, kClass);
	subClass = CFG_READ(b, kSubclass);

	pciInfo.seg = seg;
	pciInfo.bus = bus;
	pciInfo.slot = slot;
	pciInfo.fun = fun;

	pcidev = new (kmem_general) PCIDevice(this, pciInfo);
}

PCIBus::PCIBus(AcpiPC *provider, uint8_t seg, uint8_t bus)
{
	kmem_asprintf(&objhdr.name, "pcibus%d:%d", seg, bus);
	attach(provider);

	for (unsigned slot = 0; slot < 32; slot++) {
		size_t nFun = 1;

		if (laihost_pci_readw(seg, bus, slot, 0, kVendorID) == 0xffff)
			continue;

		if (laihost_pci_readb(seg, bus, slot, 0, kHeaderType) &
		    (1 << 7))
			nFun = 8;

		for (unsigned fun = 0; fun < nFun; fun++)
			doFunction(seg, bus, slot, fun);
	}
}