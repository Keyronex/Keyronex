/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 20 2023.
 */

#ifndef KRX_VIOFAM_VIONET_HH
#define KRX_VIOFAM_VIONET_HH

#include "lwip/netif.h"

#include "viodev.hh"

class VirtIONIC : VirtIODevice {
	/*! VirtIO-FS configuration */
	struct virtio_net_config *cfg;
	/*! Virtqueue 0 - transmit */
	virtio_queue tx_vq;
	/*! Virtqueue 1 - receive */
	virtio_queue rx_vq;

	/*! Pbuf transmission queue. */
	STAILQ_HEAD(, pbuf) pbuf_txq;

	/*! Nethdrs page. */
	vm_page_t *nethdrs_page;
	/*! Packet buffer pages - 64x 2KiB for RX and TX queues. */
	vm_page_t *packet_bufs_pages[64];

	struct pbuf_rx {
		struct pbuf_custom pbuf;
		/*! nethdr descriptor ID of */
		uint16_t hdr_desc_id;
		/* whether this is being freed from within processBuffers() */
		bool locked;
	};

	/*! Pre-allocated custom pbufs. One for each RX queue entry. */
	pbuf_rx pbufs[64];

	/*! Network interface state. */
	struct netif nic;

	/*! Free RX pbuf callback. */
	static void freeRXPBuf(struct pbuf *p);
	/*! Initialise netif callback. */
	static err_t netifInit(struct netif *netif);
	/*! Output a packet netif callback. */
	static err_t netifOutput(struct netif *netif, struct pbuf *p);

	void intrDpc();
	void processUsed(virtio_queue *queue, struct vring_used_elem *e);
	void processUsedOnRX(struct vring_used_elem *e);
	void processUsedOnTX(struct vring_used_elem *e);

	/* Initialise the receive virtqueue. */
	void initRXQueue();
	/* Try to send a pbuf immediately. Returns 1 if sent. */
	int trySend(struct pbuf *p);
	/* Try to start pending transmits. */
	void tryStartRequests();

    public:
	VirtIONIC(PCIDevice *provider, pci_device_info &info);
};

#endif /* KRX_VIOFAM_VIONET_HH */
