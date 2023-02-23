/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#include "viodisk.hh"

static unsigned sequence_num = 0;

VirtIODisk::VirtIODisk(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	int r;

	kmem_asprintf(&objhdr.name, "viodisk%d", sequence_num++);

	cfg = (virtio_blk_config *)device_cfg;

	r = setupQueue(&io_queue, 0);
	if (r != 0) {
		DKDevLog(this, "failed to setup queue: %d\n", r);
	}

	num_queues = 1;

	r = enableDevice();
	if (r != 0) {
		DKDevLog(this, "failed to enable device: %d\n", r);
	}

	attach(provider);
	for (;; ) ;
}

void
VirtIODisk::intrDpc()
{
}

void
VirtIODisk::processUsed(virtio_queue *queue, struct vring_used_elem *e)
{
}