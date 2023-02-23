/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#ifndef MLX_VIOFAM_VIODISK_HH
#define MLX_VIOFAM_VIODISK_HH

#include "viodev.hh"
#include "dev/virtio_blk.h"

class VirtIODisk : VirtIODevice {
    /*! Block device configuration. */
    virtio_blk_config *cfg;
    /*! I/O queue - number 0. */
    virtio_queue io_queue;

	void intrDpc();
	void processUsed(virtio_queue *queue, struct vring_used_elem *e);

    public:
	VirtIODisk(PCIDevice *provider, pci_device_info &info);
};

#endif /* MLX_VIOFAM_VIODISK_HH */
