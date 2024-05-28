#include "DKNIC.h"
#include "kdk/kmem.h"
#include "lwip/def.h"
#include "lwip/err.h"
#include "lwip/netifapi.h"
#include "lwip/prot/etharp.h"
#include "netif/etharp.h"
#include "netif/ethernet.h"

#if 1
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

	nic->mtu = 2048;
	nic->hwaddr_len = ETHARP_HWADDR_LEN;
	nic->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
	    NETIF_FLAG_LINK_UP;

	nic->name[0] = 'v';
	nic->name[1] = 't';
	nic->output = etharp_output;
	nic->linkoutput = netifOutput;

	return ERR_OK;
}

- (void)setupNetif
{
	err_t err;

	memcpy(&m_netif.hwaddr, self->m_mac, sizeof(self->m_mac));

	err = netifapi_netif_add(&m_netif, NULL, NULL, NULL, self, netifInit,
	    ethernet_input);
	kassert(err == ERR_OK);

	netif_set_default(&m_netif);
}

- (void)setLinkUp:(BOOL)up speed:(size_t)mbits fullDuplex:(BOOL)duplex
{
	err_t err;
	ip4_addr_t ip, netmask, gw;

	if (up)
		DKDevLog(self, "Link up (%luMb %s Duplex)\n", mbits,
		    duplex ? "Full" : "Half");
	else
		DKDevLog(self, "Link down\n");

	ip.addr = ipaddr_addr(IP_ADDR);
	netmask.addr = ipaddr_addr("255.255.255.0");
	gw.addr = ipaddr_addr(GW_ADDR);

	err = netifapi_netif_set_addr(&m_netif, &ip, &netmask, &gw);
	if (err != ERR_OK) {
		DKDevLog(self, "Failed to set interface IP: %d\n", err);
	}

	if (up) {
		err = netifapi_netif_set_up(&m_netif);
		if (err != ERR_OK) {
			DKDevLog(self, "Failed to set interface up: %d\n", err);
		}
	}
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

- (void)queueReceivedDataForProcessing:(void *)data
				length:(size_t)length
				    id:(uint16_t)id
{
	struct ethframe {
		uint8_t dst[6];
		uint8_t src[6];
		uint16_t type;
	} *header = data;
	struct pbuf_rx *p;
	err_t err;

	kprintf("(DEST " MAC_FMT " SRC " MAC_FMT " TYPE %x)\n",
	    MAC_ARGS(header->dst), MAC_ARGS(header->src),
	    lwip_ntohs(header->type));

	p = &m_rxPbufs[id];

	pbuf_alloced_custom(PBUF_RAW, length, PBUF_REF, &p->pbuf, data,
	    m_rxBufSize);
	p->pbuf.custom_free_function = freeRXPBuf;
	p->hdr_desc_id = id;
	p->pbuf.pbuf.if_idx = netif_get_index(&m_netif);
	p->locked = false;

	err = tcpip_input(&p->pbuf.pbuf, &m_netif);
	if (err != ERR_OK) {
		p->locked = true;
		pbuf_free(&p->pbuf.pbuf);
		DKDevLog(self, "Packet dropped: %d\n", err);
	}
}

@end
