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

	/*!
	 * Processed packets awaiting to be dispatched by tryStartPackets().
	 * Protected by io_queue->spinlock.
	 */
	TAILQ_HEAD(, iop) pending_packets;

	/* protected by queue->spinlock */
	TAILQ_HEAD(, vioblk_request) free_reqs;
	/* protected by queue->spinlock */
	TAILQ_HEAD(, vioblk_request) in_flight_reqs;

	/*!
	 * @brief Send a request to the controller.
	 * @param req one of VIRTIO_BLK_T_IN, VIRTIO_BLK_T_OUT, etc.
	 */
	int commonRequest(int req, size_t nblocks, unsigned block,
	    vm_mdl_t *buffer);

	/*!
	 * @brief ISR DPC routine
	 *
	 * Processes used ring & tries to start new packets.
	 */
	void intrDpc();

	/*!
	 * Processes a used descriptor - callback from processVirtQueue().
	 */
	void processUsed(virtio_queue *queue, struct vring_used_elem *e);

	/*!
	 * @brief Try to dispatch pending packets.
	 *
	 * @pre io_queue->spinlock shall be locked.
	 */
	void tryStartPackets();

    public:
	VirtIODisk(PCIDevice *provider, pci_device_info &info);
};

#endif /* MLX_VIOFAM_VIODISK_HH */
