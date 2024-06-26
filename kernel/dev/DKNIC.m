#include "DKNIC.h"
#include "kdb/kdb_udp.h"
#include "kdk/kmem.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/prot/etharp.h"
#include "net/net.h"
#include "netif/etharp.h"
#include "netif/ethernet.h"

#if 0
#define IP_ADDR "192.168.178.212"
#define GW_ADDR "192.168.178.1"
#else
#define IP_ADDR "10.0.2.15"
#define GW_ADDR "10.0.2.2"
#endif

@implementation DKNIC

- (void)setupForQueueLength:(size_t)size rxBufSize:(size_t)rxBufSize
{
	m_queueLength = size;
	m_rxBufSize = rxBufSize;

	m_rxPbufs = kmem_alloc(sizeof(struct pbuf_rx) * m_queueLength);

	if (strcmp([self devName], kdb_devname) == 0) {
		kassert(kdb_nic == NULL);
		kdb_nic = self;

		m_kdbAttached = true;
		DKDevLog(self, "Using for Kernel Debugger accesss");
	} else {
		m_kdbAttached = false;
	}
}

static err_t
netifOutput(struct netif *nic, struct pbuf *p)
{
	DKNIC *DKNIC = nic->state;
	kassert(DKNIC);
	[DKNIC submitPacket:p];
	return ERR_OK;
}

err_t
netifInit(struct netif *nic)
{
	DKNIC *self = (DKNIC *)nic->state;

	(void)self;

	nic->mtu = 2048;
	nic->hwaddr_len = ETHARP_HWADDR_LEN;
	nic->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
	    NETIF_FLAG_LINK_UP;

	nic->name[0] = 'e';
	nic->name[1] = 'm';
	nic->output = etharp_output;
	nic->linkoutput = netifOutput;

	return ERR_OK;
}

- (void)setupNetif
{
	ipl_t ipl;

	memcpy(&m_netif.hwaddr, self->m_mac, sizeof(self->m_mac));

	ipl = LOCK_LWIP();
	netif_add(&m_netif, NULL, NULL, NULL, self, netifInit, ethernet_input);
	netif_set_default(&m_netif);
	UNLOCK_LWIP(ipl);
}

- (void)setLinkUp:(BOOL)up speed:(size_t)mbits fullDuplex:(BOOL)duplex
{
	ip4_addr_t ip, netmask, gw;
	ipl_t ipl;

	if (up)
		DKDevLog(self, "Link up (%zuMb %s Duplex)\n", mbits,
		    duplex ? "Full" : "Half");
	else
		DKDevLog(self, "Link down\n");

	ip.addr = ipaddr_addr(IP_ADDR);
	netmask.addr = ipaddr_addr("255.255.255.0");
	gw.addr = ipaddr_addr(GW_ADDR);

	ipl = LOCK_LWIP();
	netif_set_addr(&m_netif, &ip, &netmask, &gw);
	if (up)
		netif_set_up(&m_netif);
	UNLOCK_LWIP(ipl);
}

static void
freeRXPBuf(struct pbuf *p)
{
	struct netif *netif;
	DKNIC *self;
	struct pbuf_rx *pbuf = (struct pbuf_rx *)p;

	p->ref = 1;

	netif = netif_get_by_index(p->if_idx);
	kassert(netif != NULL);
	self = (DKNIC *)netif->state;
	kassert(self != NULL);

	[self completeProcessingOfRxIndex:pbuf->hdr_desc_id
				   locked:pbuf->locked];
}

- (struct pbuf *)debugPBufForData:(void *)data
			   length:(size_t)length
			       id:(uint16_t)id
{
	struct pbuf_rx *p;

	p = &m_rxPbufs[id];

	pbuf_alloced_custom(PBUF_RAW, length, PBUF_REF, &p->pbuf, data,
	    m_rxBufSize);
	p->pbuf.custom_free_function = freeRXPBuf;
	p->hdr_desc_id = id;
	p->pbuf.pbuf.if_idx = netif_get_index(&m_netif);
	p->locked = true;
	p->netif = &m_netif;

	return &p->pbuf.pbuf;
}

- (void)queueReceivedDataForProcessing:(void *)data
				length:(size_t)length
				    id:(uint16_t)id
{
	struct pbuf_rx *p;

#if TRACE_RX
	struct ethframe {
		uint8_t dst[6];
		uint8_t src[6];
		uint16_t type;
	} *header = data;

	kprintf("(DEST " MAC_FMT " SRC " MAC_FMT " TYPE %x)\n",
	    MAC_ARGS(header->dst), MAC_ARGS(header->src),
	    lwip_ntohs(header->type));
#endif

	if (m_kdbAttached) {
		memcpy(kdb_udp_rx_pbuf.payload, data, length);
		kdb_udp_rx_pbuf.len = kdb_udp_rx_pbuf.tot_len = length;
		[self completeProcessingOfRxIndex:id locked:YES];
		kdbudp_check_packet();
		return;
	}

	p = &m_rxPbufs[id];

	pbuf_alloced_custom(PBUF_RAW, length, PBUF_REF, &p->pbuf, data,
	    m_rxBufSize);
	p->pbuf.custom_free_function = freeRXPBuf;
	p->hdr_desc_id = id;
	p->pbuf.pbuf.if_idx = netif_get_index(&m_netif);
	p->locked = false;
	p->netif = &m_netif;

	ksk_packet_in(p);
}

- (void)submitPacket:(struct pbuf *)pkt
{
	kfatal("Subclass responsibility\n");
}

- (void)completeProcessingOfRxIndex:(size_t)index locked:(BOOL)isLocked
{
	kfatal("Subclass responsibility\n");
}

- (BOOL)debuggerPoll
{
	kfatal("Subclass responsibility\n");
}

- (void)debuggerTransmit:(struct pbuf *)pbuf
{
	kfatal("Subclass responsibility\n");
}

@end
