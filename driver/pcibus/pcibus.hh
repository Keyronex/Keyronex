/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */

#ifndef MLX_PCIBUS_PCIBUS_HH
#define MLX_PCIBUS_PCIBUS_HH

#include "../acpipc/acpipc.hh"

struct pci_device_info {
	uint16_t seg;
	uint8_t bus;
	uint8_t slot;
	uint8_t fun;

	uint8_t pin;
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
};

#endif /* MLX_PCIBUS_PCIBUS_HH */
