#include <sys/types.h>

#include <kern/kmem.h>
#include <vm/vm.h>

#include <nanokern/thread.h>

#include "VirtIONetwork.h"
#include "dev/VirtIOFamily/virtioreg.h"
#include "virtio_net.h"

#define VIRTIO_NET_Q_RX 0
#define VIRTIO_NET_Q_TX 1
#define VIRTIO_NET_Q_CTRL 2

@implementation VirtIONetwork

- (void)fillRXQueue
{
	nethdrs_page = vm_pagealloc(true, &vm_pgdevbufq);
	vaddr_t nethdr_rx_pagebase = (vaddr_t)P2V(nethdrs_page->paddr);

	for (int i = 0 ; i < 64; i++) {
		packet_bufs_pages[i] = vm_pagealloc(true, &vm_pgdevbufq);
	}

	for (int i = 0; i < rx_queue.length / 2; i++) {
		uint16_t			 dhdridx, ddataidx;
		struct vring_desc		*dhdr, *ddata;
		struct virtio_net_hdr_mrg_rxbuf *hdr =
		    (void *)(nethdr_rx_pagebase + i * sizeof(*hdr));

		/* how shall we know which net_hdrs are used?? maybe no prob? */

		dhdridx = [self allocateDescNumOnQueue:&rx_queue];
		ddataidx = [self allocateDescNumOnQueue:&rx_queue];

		dhdr = &QUEUE_DESC_AT(&rx_queue, dhdridx);
		ddata = &QUEUE_DESC_AT(&rx_queue, ddataidx);

		dhdr->len = sizeof(*hdr);
		dhdr->addr = (uint64_t)V2P(hdr);
		dhdr->next = ddataidx;
		dhdr->flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;

		ddata->len = 2048;
		ddata->addr = packet_bufs_pages[i / 2]->paddr + (i % 2) * 2048;
		ddata->flags = VRING_DESC_F_WRITE;

		[self submitDescNum:dhdridx toQueue:&rx_queue];
	}
}

- initWithVirtIOInfo:(struct dk_virtio_info *)vioInfo
{
	self = [super initWithVirtIOInfo:vioInfo];

	kmem_asprintf(&m_name, "VirtIONetwork0");

	/* TODO(med): for non-qemu we might check VIRTIO_BLK_F_SIZE_MAX too */
	if (![self exchangeFeatures:VIRTIO_NET_F_MAC | VIRTIO_NET_F_STATUS]) {
		DKDevLog(self, "Feature exchange failed.");
		[self dealloc];
		return nil;
	}

	net_cfg = info.device_cfg;

	info.num_queues = 2;
	info.queues = kmem_alloc(sizeof(void*) *2);
	info.queues[0] = &rx_queue;
	info.queues[1] = &tx_queue;

	[self setupQueue:&rx_queue index:VIRTIO_NET_Q_RX];
	[self setupQueue:&tx_queue index:VIRTIO_NET_Q_TX];
	[self fillRXQueue];

	[self enableDevice];

	[self registerDevice];

	return self;
}

- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)aQueue
{
	for (;;) ;
	//fatal("not handled yet\n");
}

@end
