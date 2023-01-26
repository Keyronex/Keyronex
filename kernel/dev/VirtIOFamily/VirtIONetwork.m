#include <sys/types.h>

#include <kern/kmem.h>
#include <vm/vm.h>

#include <devicekit/DKNetwork.h>
#include <errno.h>
#include <nanokern/thread.h>

#include "VirtIONetwork.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/etharp.h"
#include "lwip/ethip6.h"
#include "lwip/mem.h"
#include "lwip/netdb.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"
#include "lwip/snmp.h"
#include "lwip/stats.h"
#include "md/spl.h"
#include "netif/ethernet.h"
#include "netif/ppp/pppoe.h"
#include "virtio_net.h"

#define VIRTIO_NET_Q_RX 0
#define VIRTIO_NET_Q_TX 1
#define VIRTIO_NET_Q_CTRL 2

/* wtf clang-format? */
/* clang-format off */
@interface VirtIONetwork (LWIP)
/* clang-format on */

- (void)lwipInit:(struct netif *)netif;
- (err_t)lwipTransmit:(struct pbuf *)p;

@end

/* lwip integration */
static err_t
vif_init(struct netif *netif)
{
	VirtIONetwork *net = netif->state;
	[net lwipInit:netif];
	return ERR_OK;
}

static err_t
vif_output(struct netif *netif, struct pbuf *p)
{
	VirtIONetwork *net = netif->state;
	return [net lwipTransmit:p];
}

@implementation VirtIONetwork

- (void)lwipInit:(struct netif *)a_netif
{
	kassert(a_netif == &netif);
	netif.mtu = 2048;
	netif.hwaddr_len = ETHARP_HWADDR_LEN;
	memcpy(netif.hwaddr, net_cfg->mac, sizeof(net_cfg->mac));
	netif.flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
	    NETIF_FLAG_LINK_UP;

	MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd,
	    LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

	netif.name[0] = 'v';
	netif.name[1] = 't';
	netif.output = etharp_output;
	netif.linkoutput = vif_output;
}

- (err_t)lwipTransmit:(struct pbuf *)p
{
	vaddr_t nethdr_tx_pagebase = (vaddr_t)P2V(nethdrs_page->paddr) +
	    64 * sizeof(struct virtio_net_hdr_mrg_rxbuf);
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	struct vring_desc		*dhdr, *ddata;
	uint16_t			 dhdridx, ddataidx;
	uint16_t			 i;

	dhdridx = [self allocateDescNumOnQueue:&tx_queue];
	ddataidx = [self allocateDescNumOnQueue:&tx_queue];

	dhdr = &QUEUE_DESC_AT(&tx_queue, dhdridx);
	ddata = &QUEUE_DESC_AT(&tx_queue, ddataidx);

	i = dhdridx / 2;

	hdr = (void *)(nethdr_tx_pagebase + i * sizeof(*hdr));
	hdr->num_buffers = 1;

	dhdr->len = sizeof(*hdr);
	dhdr->addr = (uint64_t)V2P(hdr);
	dhdr->next = ddataidx;
	dhdr->flags = VRING_DESC_F_NEXT;

	ddata->len = p->tot_len;
	ddata->addr = packet_bufs_pages[32 + i / 2]->paddr + (i % 2) * 2048;
	ddata->flags = 0;

	memcpy(P2V(ddata->addr), p->payload, p->len);
	kassert(!p->next);

	ipl_t ipl = nk_spinlock_acquire_at(&tx_queue.spinlock, kSPLBIO);
	[self submitDescNum:dhdridx toQueue:&tx_queue];
	[self notifyQueue:&tx_queue];
	nk_spinlock_release(&tx_queue.spinlock, ipl);

	MIB2_STATS_NETIF_ADD(netif, ifoutoctets, p->tot_len);
	if (((u8_t *)p->payload)[0] & 1) {
		/* multicast */
		MIB2_STATS_NETIF_INC(netif, ifoutnucastpkts);
	} else {
		/* unicast */
		MIB2_STATS_NETIF_INC(netif, ifoutucastpkts);
	}

#if ETH_PAD_SIZE
	pbuf_add_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

	LINK_STATS_INC(link.xmit);

	return ERR_OK;
}

- (void)fillRXQueue
{
	nethdrs_page = vm_pagealloc(true, &vm_pgdevbufq);
	vaddr_t nethdr_rx_pagebase = (vaddr_t)P2V(nethdrs_page->paddr);

	for (int i = 0; i < 64; i++) {
		packet_bufs_pages[i] = vm_pagealloc(true, &vm_pgdevbufq);
	}

	for (int i = 0; i < rx_queue.length / 2; i++) {
		uint16_t			 dhdridx, ddataidx;
		struct vring_desc		*dhdr, *ddata;
		struct virtio_net_hdr_mrg_rxbuf *hdr =
		    (void *)(nethdr_rx_pagebase + i * sizeof(*hdr));

		memset(hdr, 0x0, sizeof(*hdr));
		hdr->num_buffers = 1;

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

	[self notifyQueue:&rx_queue];
}

- initWithVirtIOInfo:(struct dk_virtio_info *)vioInfo
{
	self = [super initWithVirtIOInfo:vioInfo];

	kmem_asprintf(&m_name, "VirtIONetwork0");

	if (![self exchangeFeatures:VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MAC |
		   VIRTIO_NET_F_STATUS]) {
		DKDevLog(self, "Feature exchange failed.");
		[self dealloc];
		return nil;
	}

	net_cfg = info.device_cfg;

	info.num_queues = 2;
	info.queues = kmem_alloc(sizeof(void *) * 2);
	info.queues[0] = &rx_queue;
	info.queues[1] = &tx_queue;

	[self setupQueue:&rx_queue index:VIRTIO_NET_Q_RX];
	[self setupQueue:&tx_queue index:VIRTIO_NET_Q_TX];
	[self fillRXQueue];

	netifapi_netif_add(&netif, NULL, NULL, NULL, self, vif_init,
	    ethernet_input);

	[PCIBus setInterruptsOf:&info.pciInfo enabled:YES];
	[self enableDevice];

	netifapi_netif_set_up(&netif);

	[self registerDevice];

	DKLogAttachExtra(self, "MAC address: " MAC_FMT, net_cfg->mac[0],
	    net_cfg->mac[1], net_cfg->mac[2], net_cfg->mac[3], net_cfg->mac[4],
	    net_cfg->mac[5]);

#if 1
	ktimer_t timer;
	nk_timer_init(&timer);
	nk_timer_set(&timer, (uint64_t)NS_PER_S * 30);

	nk_wait(&timer, "before_lookup", false, false, -1);

	asm("cli");
	netifapi_dhcp_start(&netif);
	asm("sti");

	nk_timer_set(&timer, (uint64_t)NS_PER_S * 4);

	nk_wait(&timer, "before_lookup", false, false, -1);

	kprintf("IP: %x\n", netif.ip_addr.addr);

	struct addrinfo *res;
	asm("cli");
	kprintf("Lookup addr of <google.com>\n");
	int r = lwip_getaddrinfo("google.com", "443", 0, &res);
	kprintf("getaddrinfo returned %d, errno %d\n", r, errno);
	asm("sti");

	if (res == NULL) {
		kprintf("failed to lookup host\n")
	} else {
		char		    ipv4[INET_ADDRSTRLEN];
		struct sockaddr_in *addr4;
		addr4 = (struct sockaddr_in *)res->ai_addr;
		inet_ntop(AF_INET, &addr4->sin_addr, ipv4, INET_ADDRSTRLEN);
		printf("IP: %s\n", ipv4);
	}
#endif

	return self;
}

- (void)processBufferOnRXQueue:(struct vring_used_elem *)e
{
	struct vring_desc *dhdr, *ddata;
	uint16_t	   ddataidx;
	vaddr_t		   dataaddr;

	dhdr = &QUEUE_DESC_AT(&rx_queue, e->id);
	kassert(dhdr->flags & VRING_DESC_F_NEXT);
	ddataidx = dhdr->next;
	ddata = &QUEUE_DESC_AT(&rx_queue, ddataidx);

	dataaddr = (vaddr_t)P2V(ddata->addr);

	struct pbuf *p;

	/* point pbuf into the buffer */
	p = pbuf_alloc_reference((void *)dataaddr, e->len, PBUF_REF);
	if (p != NULL) {
		MIB2_STATS_NETIF_ADD(netif, ifinoctets, p->tot_len);
		if (((u8_t *)p->payload)[0] & 1) {
			MIB2_STATS_NETIF_INC(netif, ifinnucastpkts);
		} else {
			MIB2_STATS_NETIF_INC(netif, ifinucastpkts);
		}
		LINK_STATS_INC(link.recv);

		if (netif.input(p, &netif) != ERR_OK) {
			DKDevLog(self, "ip input error");
			pbuf_free(p);
			p = NULL;
		}
	} else {
		kprintf("packet dropped\n");
		LINK_STATS_INC(link.memerr);
		LINK_STATS_INC(link.drop);
		MIB2_STATS_NETIF_INC(netif, ifindiscards);
	}

	/* we never free descs on the RX queue, they are permanent */
	[self submitDescNum:e->id toQueue:&rx_queue];
}

- (void)processBufferOnTXQueue:(struct vring_used_elem *)e
{
	struct vring_desc *dhdr; //*ddata;
	uint16_t	   ddataidx;

	dhdr = &QUEUE_DESC_AT(&tx_queue, e->id);
	kassert(dhdr->flags & VRING_DESC_F_NEXT);
	ddataidx = dhdr->next;

	/* just return the descs for future use */
	[self freeDescNum:e->id onQueue:&tx_queue];
	[self freeDescNum:ddataidx onQueue:&tx_queue];
}

- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)aQueue
{
	if (aQueue == &rx_queue)
		[self processBufferOnRXQueue:e];
	else
		[self processBufferOnTXQueue:e];
}

@end
