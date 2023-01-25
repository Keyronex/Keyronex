
#ifndef VIRTIONETWORK_H_
#define VIRTIONETWORK_H_

#include <dev/VirtIOFamily/VirtIODevice.h>

#include <devicekit/DKDisk.h>

#include "lwip/netif.h"

@interface VirtIONetwork : VirtIODevice {
	struct virtio_net_config *net_cfg;
	dk_virtio_queue_t tx_queue, rx_queue;

	/* page holding nethdrs */
	vm_page_t *nethdrs_page;

	/*!
	 * pages holding packet buffers. we need 64 each for RX and TX queue.
	 * we use a 2048-byte buffer, so we need 64 pages.
	 */
	vm_page_t *packet_bufs_pages[64];

	/*!
	 * lwip
	 */
	struct netif netif;
}

- initWithVirtIOInfo:(struct dk_virtio_info *)info;

//-(void) transmit:


@end

#endif /* VIRTIONETWORK_H_ */
