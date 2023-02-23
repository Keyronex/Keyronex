/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#ifndef MLX_VIOFAM_VIODISK_HH
#define MLX_VIOFAM_VIODISK_HH

#include "viodev.hh"

class VirtIODisk : VirtIODevice {
    public:
	VirtIODisk(PCIDevice *provider, pci_device_info &info);
};

#endif /* MLX_VIOFAM_VIODISK_HH */
