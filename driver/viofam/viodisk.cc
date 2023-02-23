/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#include "viodisk.hh"

static unsigned sequence_num = 0;

VirtIODisk::VirtIODisk(PCIDevice *provider, pci_device_info &info) : VirtIODevice(provider, info)
{
	kmem_asprintf(&objhdr.name, "viodisk%d", sequence_num++);
	attach(provider);
}
