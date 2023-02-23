/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */

#include "acpispec/resources.h"
#include "kdk/object.h"
#include "lai/helpers/pci.h"

#include "../viofam/viodisk.hh"
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

#define INFO_ARGS(INFO) (INFO)->seg, (INFO)->bus, (INFO)->slot, (INFO)->fun
#define ENABLE_CMD_FLAG(INFO, FLAG)                   \
	laihost_pci_writew(INFO_ARGS(INFO), kCommand, \
	    laihost_pci_readw(INFO_ARGS(INFO), kCommand) | (FLAG))
#define DISABLE_CMD_FLAG(INFO, FLAG)                  \
	laihost_pci_writew(INFO_ARGS(INFO), kCommand, \
	    laihost_pci_readw(INFO_ARGS(INFO), kCommand) & ~(FLAG))

PCIDevice::PCIDevice(PCIBus *provider, pci_device_info &info)
    : info(info)
{
	acpi_resource_t res;
	int r;

	kmem_asprintf(&objhdr.name, "pcidev%d:%d:%d.%d", info.seg, info.bus,
	    info.slot, info.fun);
	attach(provider);

	if (info.pin != 0) {
		r = lai_pci_route_pin(&res, INFO_ARGS(&info), info.pin);
		if (r != LAI_ERROR_NONE) {
			kfatal("failed to route pin!\n");
		} else {
			info.gsi = res.base;
			info.lopol = res.irq_flags & ACPI_SMALL_IRQ_ACTIVE_LOW;
			info.edge = res.irq_flags &
			    ACPI_SMALL_IRQ_EDGE_TRIGGERED;
		}
	}

	if (info.vendorId == 0x1af4 && info.deviceId == 0x1001) {
		new (kmem_general) VirtIODisk(this, info);
	}
}

void
PCIDevice::enableMemorySpace(pci_device_info &info)
{
	ENABLE_CMD_FLAG(&info, 0x1 | 0x2);
}

void
PCIDevice::enableBusMastering(pci_device_info &info)
{
	ENABLE_CMD_FLAG(&info, 0x4);
}

void
PCIDevice::setInterrupts(pci_device_info &info, bool enabled)
{
	if (enabled)
		DISABLE_CMD_FLAG(&info, (1 << 10));
	else
		ENABLE_CMD_FLAG(&info, (1 << 10));
}

void
PCIDevice::enumerateCapabilities(pci_device_info &info,
    void (*callback)(pci_device_info *info, voff_t cap, void *arg),
    void *userData)
{
	if (PCIINFO_CFG_READ(w, &info, kStatus) & (1 << 4)) {
		voff_t pCap = PCIINFO_CFG_READ(b, &info, kCapabilitiesPointer);

		while (pCap != 0) {
			callback(&info, pCap, userData);
			pCap = PCIINFO_CFG_READ(b, &info, pCap + 1);
		}
	}
}

paddr_t
PCIDevice::getBar(pci_device_info &info, uint8_t num)
{
	return (paddr_t)((uintptr_t)laihost_pci_readd(INFO_ARGS(&info),
			     kBaseAddress0 + sizeof(uint32_t) * num) &
	    0xfffffff0);
}

void
PCIBus::doFunction(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun)
{
	pci_device_info pciInfo;
	PCIDevice *pcidev;

#define CFG_READ(WIDTH, OFFSET) \
	laihost_pci_read##WIDTH(seg, bus, slot, fun, OFFSET)

	pciInfo.vendorId = CFG_READ(w, kVendorID);
	if (pciInfo.vendorId == 0xffff)
		return;
	pciInfo.deviceId = CFG_READ(w, kDeviceID);
	pciInfo.klass = CFG_READ(b, kClass);
	pciInfo.subClass = CFG_READ(b, kSubclass);

	pciInfo.seg = seg;
	pciInfo.bus = bus;
	pciInfo.slot = slot;
	pciInfo.fun = fun;
	pciInfo.pin = CFG_READ(b, kInterruptPin);

	pcidev = new (kmem_general) PCIDevice(this, pciInfo);
	(void)pcidev;
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