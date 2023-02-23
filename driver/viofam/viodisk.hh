/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#ifndef MLX_VIOFAM_VIODISK_HH
#define MLX_VIOFAM_VIODISK_HH

#include "dev/virtio_blk.h"

#include "viodev.hh"

class VirtIODisk : VirtIODevice {
	/*! Block device configuration. */
	virtio_blk_config *cfg;
	/*! I/O queue - number 0. */
	virtio_queue io_queue;

	/* protected by queue->spinlock */
	TAILQ_HEAD(, vioblk_request) free_reqs;
	/* protected by queue->spinlock */
	TAILQ_HEAD(, vioblk_request) in_flight_reqs;

	void intrDpc();
	void processUsed(virtio_queue *queue, struct vring_used_elem *e);
	int commonRequest(int req, size_t blocks, unsigned offset,
	    void *buffer);

    public:
	VirtIODisk(PCIDevice *provider, pci_device_info &info);
};

#endif /* MLX_VIOFAM_VIODISK_HH */
