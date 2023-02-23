/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */

#ifndef MLX_PCIBUS_PCIBUS_HH
#define MLX_PCIBUS_PCIBUS_HH

#include "../acpipc/acpipc.hh"

#define PCIINFO_CFG_READ(WIDTH, PCIINFO, OFFSET)                \
	laihost_pci_read##WIDTH((PCIINFO)->seg, (PCIINFO)->bus, \
	    (PCIINFO)->slot, (PCIINFO)->fun, OFFSET)

struct pci_device_info {
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

class PCIBus : public Device {
	void doFunction(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fun);

    public:
	PCIBus(AcpiPC *provider, uint8_t seg, uint8_t bus);
};

class PCIDevice : public Device {
	pci_device_info info;

    public:
	PCIDevice(PCIBus *provider, pci_device_info &info);

	static void enableMemorySpace(pci_device_info &info);
	static void enableBusMastering(pci_device_info &info);
	static void setInterrupts(pci_device_info &info, bool enabled);
	static void enumerateCapabilities(pci_device_info &info,
	    void (*callback)(pci_device_info *info, voff_t cap, void *arg),
	    void *userData);
	static paddr_t getBar(pci_device_info &info, uint8_t num);
};

#endif /* MLX_PCIBUS_PCIBUS_HH */
