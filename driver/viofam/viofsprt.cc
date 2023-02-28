/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 28 2023.
 */

#include "viofsprt.hh"

static int sequence_num = 0;

VirtIOFSPort::VirtIOFSPort(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	kmem_asprintf(&objhdr.name, "viofsprt%d", sequence_num++);
}