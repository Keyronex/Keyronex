/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#ifndef MLX_VIOFAM_VIODEV_HH
#define MLX_VIOFAM_VIODEV_HH

#include "kdk/kernel.h"
#include "kdk/vm.h"

#include "../pcibus/pcibus.hh"

struct vring_used_elem;

struct virtio_queue {
	/* queue number/index */
	uint16_t num;
	/* length of uqeue */
	uint16_t length;

	/* descriptor allocation semaphore */
	ksemaphore_t free_sem;
	/* manipulation spinlock */
	kspinlock_t spinlock;

	/* index of first free descriptor */
	uint16_t free_desc_index;
	/* last seen used index */
	uint16_t last_seen_used;

	/* page out of which desc, avail, used are allocated */
	vm_page_t *page;

	/* virtual address of descriptor array */
	struct vring_desc *desc;
	/* virtual address of driver ring */
	struct vring_avail *avail;
	/* virtual address of device ring */
	struct vring_used *used;
	/* notification offset */
	uint16_t notify_off;
};

#define QUEUE_DESC_AT(PQUEUE, IDX) ((PQUEUE)->desc[IDX])

class VirtIODevice : public Device {
	kdpc_t interrupt_dpc;

	static void intrDpc(void *arg);
	static bool intxISR(hl_intr_frame_t *frame, void *arg);
	static void enumerateCapabilitiesCallback(pci_device_info *info,
	    voff_t cap, void *arg);

	/*! DPC to process pending used elements. */
	virtual void intrDpc() = 0;

	/*!
	 * Callback to process a completion on a queue.
	 *
	 * @pre queue->lock will be locked (since this is called by
	 * processQueue)
	 */
	virtual void processUsed(virtio_queue *queue,
	    struct vring_used_elem *e) = 0;

    protected:
	pci_device_info pci_info;
	struct virtio_pci_common_cfg *m_common_cfg;
	uint8_t *isr;
	void *device_cfg;
	vaddr_t notify_base;

	uint32_t m_notify_off_multiplier;

	uint16_t num_queues;
	struct dk_virtio_queue **queues;

	VirtIODevice(PCIDevice *provider, pci_device_info &info);
	virtual ~VirtIODevice() {};

	bool exchangeFeatures(uint64_t required_mask);
	int enableDevice();

	/*! Initialise a queue and register it. */
	int setupQueue(virtio_queue *queue, uint16_t index);
	/*!
	 * Notify a queue that new entries are enqueued.
	 *
	 * @pre queue->lock held
	 */
	void notifyQueue(virtio_queue *queue);
	/*!
	 * Process used entries on a queue. Calls processUsed() for each entry.
	 *
	 * @pre queue->lock must be locked (at IPL DPC)
	 */
	void processQueue(virtio_queue *queue);

	/*!
	 * @brief Allocate a descriptor from a queue.
	 */
	uint16_t allocateDescNumOnQueue(virtio_queue *queue);
	/*!
	 * @brief Free a descriptor to a queue.
	 */
	void freeDescNumOnQueue(virtio_queue *queue, uint16_t descNum);
	/*!
	 * @brief Submit a descriptor to a queue.
	 */
	void submitDescNumToQueue(virtio_queue *queue, uint16_t descNum);
};

#endif /* MLX_VIOFAM_VIODEV_HH */
