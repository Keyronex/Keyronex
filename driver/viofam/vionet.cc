/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Mar 21 2023.
 */

#include <inttypes.h>

#include "dev/virtio_net.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "lwip/err.h"
#include "lwip/etharp.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/pbuf.h"
#include "lwip/snmp.h"
#include "lwip/stats.h"
#include "lwip/tcpip.h"
#include "netif/ethernet.h"

#include "vionet.hh"

#define TEST_IP 1

#define VIRTIO_NET_Q_RX 0
#define VIRTIO_NET_Q_TX 1
#define VIRTIO_NET_Q_CTRL 2

#define MAC_FMT_PART "%02" PRIx8
#define MAC_FMT                                                         \
	MAC_FMT_PART ":" MAC_FMT_PART ":" MAC_FMT_PART ":" MAC_FMT_PART \
		     ":" MAC_FMT_PART ":" MAC_FMT_PART

#define MAC_ARG(MAC) mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]

static int sequence_num = 0;

err_t
VirtIONIC::netifInit(struct netif *nic)
{
	VirtIONIC *self = (VirtIONIC *)nic->state;
	kassert(nic == &self->nic);
	nic->mtu = 2048;
	nic->hwaddr_len = ETHARP_HWADDR_LEN;
	memcpy(nic->hwaddr, self->cfg->mac, sizeof(self->cfg->mac));
	nic->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
	    NETIF_FLAG_LINK_UP;

	MIB2_INIT_NETIF(nic, snmp_ifType_ethernet_csmacd, 1000000000);

	nic->name[0] = 'v';
	nic->name[1] = 't';
	nic->output = etharp_output;
	nic->linkoutput = netifOutput;

	return ERR_OK;
}

err_t
VirtIONIC::netifOutput(struct netif *nic, struct pbuf *p)
{
	VirtIONIC *self = (VirtIONIC *)nic->state;
	ipl_t ipl = ke_spinlock_acquire(&self->tx_vq.spinlock);
	/* !!! lwip may have freed the pbuf; we need to copy it really */
	STAILQ_INSERT_TAIL(&self->pbuf_txq, p, stailq_entry);
	ke_dpc_enqueue(&self->interrupt_dpc);
	ke_spinlock_release(&self->tx_vq.spinlock, ipl);
	return ERR_OK;
}

VirtIONIC::VirtIONIC(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	int r;
	err_t err;

	kmem_asprintf(&objhdr.name, "vionic%d", sequence_num++);
	cfg = (virtio_net_config *)device_cfg;

	if (!exchangeFeatures(VIRTIO_F_VERSION_1)) {
		DKDevLog(this, "Feature exchange failed.\n");
		return;
	}

	r = setupQueue(&rx_vq, VIRTIO_NET_Q_RX);
	if (r != 0) {
		DKDevLog(this, "failed to setup hiprio queue: %d\n", r);
		return;
	}

	r = setupQueue(&tx_vq, VIRTIO_NET_Q_TX);
	if (r != 0) {
		DKDevLog(this, "failed to setup request queue: %d\n", r);
		return;
	}

	initRXQueue();
	STAILQ_INIT(&pbuf_txq);

	err = netifapi_netif_add(&nic, NULL, NULL, NULL, this, netifInit,
	    ethernet_input);
	if (err != ERR_OK) {
		DKDevLog(this, "Failed to add interface: %d\n", err);
	}

	r = enableDevice();
	if (r != 0) {
		DKDevLog(this, "failed to enable device: %d\n", r);
	}

	attach(provider);
	DKDevLog(this, "MAC address: " MAC_FMT "\n", cfg->mac[0], cfg->mac[1],
	    cfg->mac[2], cfg->mac[3], cfg->mac[4], cfg->mac[5]);

#if TEST_IP == 1
	ip4_addr_t ip, netmask, gw;
	ip.addr = ipaddr_addr("192.168.122.50");
	netmask.addr = ipaddr_addr("255.255.255.0");
	gw.addr = ipaddr_addr("192.168.122.1");

	err = netifapi_netif_set_addr(&nic, &ip, &netmask, &gw);
	if (err != ERR_OK) {
		DKDevLog(this, "Failed to set interface IP: %d\n", err);
	}
#endif

	err = netifapi_netif_set_up(&nic);
	if (err != ERR_OK) {
		DKDevLog(this, "Failed to set interface up: %d\n", err);
	}

#if 0
	for (;;)
		asm("pause");
#endif
}

void
VirtIONIC::initRXQueue()
{
	vaddr_t nethdr_rx_pagebase;

	vmp_page_alloc(kernel_process.map, true, kPageUseDevBuf, &nethdrs_page);

	nethdr_rx_pagebase = (vaddr_t)VM_PAGE_DIRECT_MAP_ADDR(nethdrs_page);

	for (int i = 0; i < 64; i++) {
		vmp_page_alloc(kernel_process.map, true, kPageUseDevBuf,
		    &packet_bufs_pages[i]);
	}

	for (int i = 0; i < rx_vq.length / 2; i++) {
		uint16_t dhdridx, ddataidx;
		vring_desc *dhdr, *ddata;
		virtio_net_hdr_mrg_rxbuf *hdr =
		    (virtio_net_hdr_mrg_rxbuf *)(nethdr_rx_pagebase +
			i * sizeof(*hdr));

		memset(hdr, 0x0, sizeof(*hdr));
		hdr->num_buffers = 1;

		dhdridx = allocateDescNumOnQueue(&rx_vq);
		ddataidx = allocateDescNumOnQueue(&rx_vq);

		dhdr = &QUEUE_DESC_AT(&rx_vq, dhdridx);
		ddata = &QUEUE_DESC_AT(&rx_vq, ddataidx);

		dhdr->len = sizeof(*hdr);
		dhdr->addr = (uint64_t)V2P(hdr);
		dhdr->next = ddataidx;
		dhdr->flags = VRING_DESC_F_WRITE | VRING_DESC_F_NEXT;

		ddata->len = 2048;
		ddata->addr = VM_PAGE_PADDR(packet_bufs_pages[i / 2]) +
		    (i % 2) * 2048;
		ddata->flags = VRING_DESC_F_WRITE;

		submitDescNumToQueue(&rx_vq, dhdridx);
	}

	notifyQueue(&rx_vq);
}

void
VirtIONIC::intrDpc()
{
	ipl_t ipl = ke_spinlock_acquire(&rx_vq.spinlock);
	processVirtQueue(&rx_vq);
	ke_spinlock_release(&rx_vq.spinlock, ipl);
	ipl = ke_spinlock_acquire(&tx_vq.spinlock);
	processVirtQueue(&tx_vq);
	tryStartRequests();
	ke_spinlock_release(&tx_vq.spinlock, ipl);
}

void
VirtIONIC::processUsed(virtio_queue *queue, struct vring_used_elem *e)
{
	struct vring_desc *dhdr, *ddata;
	uint16_t ddataidx;
	vaddr_t dataaddr;
	err_t err;

	dhdr = &QUEUE_DESC_AT(&rx_vq, e->id);
	kassert(dhdr->flags & VRING_DESC_F_NEXT);
	ddataidx = dhdr->next;
	ddata = &QUEUE_DESC_AT(&rx_vq, ddataidx);

	dataaddr = (vaddr_t)P2V(ddata->addr);

	auto *p = &pbufs[e->id / 2];
	pbuf_alloced_custom(PBUF_RAW, e->len, PBUF_REF, &p->pbuf,
	    (void *)dataaddr, 2048);
	p->pbuf.custom_free_function = freeRXPBuf;
	p->hdr_desc_id = e->id;
	p->pbuf.pbuf.if_idx = netif_get_index(&nic);

	err = tcpip_input(&p->pbuf.pbuf, &nic);
	if (err != ERR_OK) {
#if 0
		DKDevLog(this, "ip input error: %d; packed dropped\n", err);
#endif
		/* mark the pbuf for locked free*/
		p->locked = true;
		pbuf_free(&p->pbuf.pbuf);
		p = NULL;
		LINK_STATS_INC(link.memerr);
		LINK_STATS_INC(link.drop);
		MIB2_STATS_NETIF_INC(netif, ifindiscards);
	} else {
		MIB2_STATS_NETIF_ADD(nic, ifinoctets, p->tot_len);
		if (((u8_t *)p->pbuf.pbuf.payload)[0] & 1) {
			MIB2_STATS_NETIF_INC(nic, ifinnucastpkts);
		} else {
			MIB2_STATS_NETIF_INC(nic, ifinucastpkts);
		}
		LINK_STATS_INC(link.recv);
	}
}

void
VirtIONIC::tryStartRequests()
{
	vaddr_t nethdr_tx_pagebase = (vaddr_t)VM_PAGE_DIRECT_MAP_ADDR(
	    nethdrs_page) + 64 * sizeof(struct virtio_net_hdr_mrg_rxbuf);
	struct pbuf *p;
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	struct vring_desc *dhdr, *ddata;
	uint16_t dhdridx, ddataidx;
	uint16_t i;

again:
	if (tx_vq.nfree_descs < 2)
		return;

	p = STAILQ_FIRST(&pbuf_txq);
	if (!p)
		return;

	STAILQ_REMOVE(&pbuf_txq, p, pbuf, stailq_entry);

	dhdridx = allocateDescNumOnQueue(&tx_vq);
	ddataidx = allocateDescNumOnQueue(&tx_vq);

	dhdr = &QUEUE_DESC_AT(&tx_vq, dhdridx);
	ddata = &QUEUE_DESC_AT(&tx_vq, ddataidx);

	i = dhdridx / 2;

	hdr = (virtio_net_hdr_mrg_rxbuf *)(nethdr_tx_pagebase +
	    i * sizeof(*hdr));
	hdr->num_buffers = 1;

	dhdr->len = sizeof(*hdr);
	dhdr->addr = (uint64_t)V2P(hdr);
	dhdr->next = ddataidx;
	dhdr->flags = VRING_DESC_F_NEXT;

	ddata->len = p->tot_len;
	ddata->addr = VM_PAGE_PADDR(packet_bufs_pages[32 + i / 2]) +
	    (i % 2) * 2048;
	ddata->flags = 0;

	kassert(p->tot_len <= 2048);
	pbuf_copy_partial(p, P2V(ddata->addr), 2048, 0);

	submitDescNumToQueue(&tx_vq, dhdridx);
	notifyQueue(&tx_vq);

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

	goto again;
}

void
VirtIONIC::freeRXPBuf(struct pbuf *p)
{
	ipl_t ipl;
	struct netif *netif;
	VirtIONIC *self;
	pbuf_rx *pbuf = (pbuf_rx *)p;

	netif = netif_get_by_index(p->if_idx);
	kassert(netif != NULL);
	self = (VirtIONIC *)netif->state;
	kassert(self != NULL);

	if (!pbuf->locked)
		ipl = ke_spinlock_acquire(&self->rx_vq.spinlock);

	self->submitDescNumToQueue(&self->rx_vq, pbuf->hdr_desc_id);

	if (!pbuf->locked)
		ke_spinlock_release(&self->rx_vq.spinlock, ipl);
}
