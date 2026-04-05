/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Wed Jan 07 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file VirtIONIC.m
 * @brief VirtIO NIC driver.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/vm.h>

#include <devicekit/virtio/VirtIONIC.h>
#include <devicekit/virtio/virtio_net.h>
#include <devicekit/virtio/virtioreg.h>
#include <stdint.h>

#define VIRTIO_NET_Q_RX 0
#define VIRTIO_NET_Q_TX 1

/*! maximum packet size, inclusive of virtio header */
#define VIONIC_RX_BUF_SIZE 2048

/*! maximum number of physical breaks in a single TX */
#define VIONIC_MAX_TX_BREAKS 16

/* RX request structure - a receive buffer */
struct vionic_rx_req {
	/* Linkage for free or in-flight list */
	TAILQ_ENTRY(vionic_rx_req) queue_entry;
	/* First descriptor ID for this buffer */
	uint16_t first_desc_id;
	/* Receive buffer (header + packet data) */
	uint8_t *buffer;
	/* Physical address of buffer */
	paddr_t buffer_paddr;
};

/* TX request - free or in-flight transmit operation */
struct vionic_tx_req {
	/* Linkage for in-flight or free list */
	TAILQ_ENTRY(vionic_tx_req) queue_entry;
	/* First descriptor ID */
	uint16_t first_desc_id;
	/* Number of descriptors used */
	uint16_t ndescs;
	/* TX header (must persist until completion) */
	struct virtio_net_hdr_v1 hdr;
};

#define DKDevLog(dev, fmt, ...) kdprintf("virtio-net: " fmt, ##__VA_ARGS__)

@implementation VirtIONIC

#define m_cfg ((volatile struct virtio_net_config *)m_transport.deviceConfig)

- (void)handleReceivedPacket:(const uint8_t *)data length:(size_t)len
{
	mblk_t *mp = str_allocb(len);
	if (mp == NULL)
		return; /* drop */

	memcpy(mp->wptr, data, len);
	mp->wptr += len;

	[self didReceivePacket:mp];
}

/* Submit an RX buffer to the receive vq. */
- (void)submitRxReq:(struct vionic_rx_req *)req
{
	uint16_t desc_id;

	desc_id = [m_transport allocateDescNumOnQueue:&m_rx_vq];
	req->first_desc_id = desc_id;

	m_rx_vq.desc[desc_id].addr = to_leu64(req->buffer_paddr);
	m_rx_vq.desc[desc_id].len = to_leu32(VIONIC_RX_BUF_SIZE);
	m_rx_vq.desc[desc_id].flags = to_leu16(VRING_DESC_F_WRITE);
	m_rx_vq.desc[desc_id].next = to_leu16(0);

	TAILQ_INSERT_TAIL(&m_rx_inflight_reqs, req, queue_entry);

	[m_transport submitDescNum:desc_id toQueue:&m_rx_vq];
}

/* Replenish all available RX buffers to the vq. */
- (void)replenishRxQueue
{
	struct vionic_rx_req *req;
	bool notify = false;

	while ((req = TAILQ_FIRST(&m_rx_free_reqs)) != NULL &&
	    m_rx_vq.nfree_descs >= 1) {
		TAILQ_REMOVE(&m_rx_free_reqs, req, queue_entry);
		[self submitRxReq:req];
		notify = true;
	}

	if (notify)
		[m_transport notifyQueue:&m_rx_vq];
}

- (instancetype)initWithTransport:(DKVirtIOTransport*) transport
{
	volatile struct virtio_net_config *cfg;
	ipl_t ipl;

	[super start];
	m_transport = transport;

	[m_transport resetDevice];

	if (![m_transport exchangeFeaturesMandatory:VIRTIO_F_VERSION_1
					   optional:NULL]) {
		DKDevLog(self, "Failed to negotiate features\n");
		return nil;
	}

	cfg = m_cfg;

	for (int i = 0; i < 6; i++)
		m_mac_address[i] = cfg->mac[i];

	DKDevLog(self, "MAC: %02x:%02x:%02x:%02x:%02x:%02x\n", m_mac_address[0],
	    m_mac_address[1], m_mac_address[2], m_mac_address[3],
	    m_mac_address[4], m_mac_address[5]);

	[m_transport setupQueue:&m_rx_vq index:VIRTIO_NET_Q_RX];
	[m_transport setupQueue:&m_tx_vq index:VIRTIO_NET_Q_TX];

	/* setup the  RX request pool */
	TAILQ_INIT(&m_rx_free_reqs);
	TAILQ_INIT(&m_rx_inflight_reqs);

	m_rx_bufs_n = m_rx_vq.length;
	m_rx_reqs = kmem_alloc(m_rx_bufs_n * sizeof(struct vionic_rx_req));
	m_rx_req_pages = kmem_alloc((roundup2(m_rx_bufs_n, 2) / 2) *
	    sizeof(vm_page_t *));

	for (size_t i = 0; i < m_rx_bufs_n; i++) {
		struct vionic_rx_req *req = &m_rx_reqs[i];
		vm_page_t *page;

		if (i % 2 == 0) {
			page = vm_page_alloc(VM_PAGE_DEV_BUFFER, 0,
			    VM_DOMID_ANY, VM_SLEEP);
			m_rx_req_pages[i / 2] = page;
		} else {
			page = m_rx_req_pages[i / 2];
		}

		req->buffer = (uint8_t *)vm_page_hhdm_addr(page);
		req->buffer_paddr = vm_page_paddr(page);

		if (i % 2 != 0) {
			req->buffer += PGSIZE / 2;
			req->buffer_paddr += PGSIZE / 2;
		}

		TAILQ_INSERT_TAIL(&m_rx_free_reqs, req, queue_entry);
	}

	/* setup the TX request pool */
	TAILQ_INIT(&m_tx_free_reqs);
	TAILQ_INIT(&m_tx_inflight_reqs);

	/*
	 * every TX needs at least 2 descriptors (header + 1 data),
	 * so allocate as many TX requests as half the vq size.
	 */
	m_tx_reqs_n = m_tx_vq.length / 2;

	m_tx_reqs = kmem_alloc(m_tx_reqs_n * sizeof(struct vionic_tx_req));

	for (size_t i = 0; i < m_tx_reqs_n; i++)
		TAILQ_INSERT_TAIL(&m_tx_free_reqs, &m_tx_reqs[i], queue_entry);

	[m_transport enableDevice];

	/* populate the RX vq with the buffers */
	ipl = ke_spinlock_enter(&m_rx_vq.spinlock);
	[self replenishRxQueue];
	ke_spinlock_exit(&m_rx_vq.spinlock, ipl);

	DKDevLog(self, "Started with %zu RX buffers, %zu TX slots\n",
	    m_rx_bufs_n, m_tx_reqs_n);

	[super setupNIC];

	return self;
}

- (int)transmitPacket:(mblk_t *)mp
{
	struct vionic_tx_req *req;
	mblk_t *m;
	uint16_t descs[VIONIC_MAX_TX_BREAKS + 1];
	size_t nsegs = 0;
	size_t i;
	ipl_t ipl;

	/* count segments; TODO: some helper function in STREAMS */
	for (m = mp; m != NULL; m = m->cont) {
		size_t seg_len = m->wptr - m->rptr;
		if (seg_len > 0)
			nsegs++;
	}

	if (nsegs == 0)
		return 0; /* nothing to send */

	if (nsegs > VIONIC_MAX_TX_BREAKS) {
		DKDevLog(self, "TX: too many segments (%zu > %d)\n", nsegs,
		    VIONIC_MAX_TX_BREAKS);
		return -EMSGSIZE;
	}

	ipl = ke_spinlock_enter(&m_tx_vq.spinlock);

	/* do we have a free TX request? */
	req = TAILQ_FIRST(&m_tx_free_reqs);
	if (req == NULL) {
		// kfatal("out of tx requests\n");
		ke_spinlock_exit(&m_tx_vq.spinlock, ipl);
		return -EAGAIN;
	}

	/* do we have enough descriptors? (1 for header + nsegs for data) */
	if (m_tx_vq.nfree_descs < nsegs + 1) {
		kfatal("out of descriptors\n");
		ke_spinlock_exit(&m_tx_vq.spinlock, ipl);
		return -EAGAIN;
	}

	TAILQ_REMOVE(&m_tx_free_reqs, req, queue_entry);

	for (size_t i = 0; i < nsegs + 1; i++)
		descs[i] = [m_transport allocateDescNumOnQueue:&m_tx_vq];

	memset(&req->hdr, 0, sizeof(req->hdr));
	req->hdr.flags = 0;
	req->hdr.gso_type = VIRTIO_NET_HDR_GSO_NONE;

	req->first_desc_id = descs[0];
	req->ndescs = nsegs + 1;

	/* first descriptor: virtio-net header */
	m_tx_vq.desc[descs[0]].addr = to_leu64(v2p((vaddr_t)&req->hdr));
	m_tx_vq.desc[descs[0]].len = to_leu32(sizeof(struct virtio_net_hdr_v1));
	m_tx_vq.desc[descs[0]].flags = to_leu16(VRING_DESC_F_NEXT);
	m_tx_vq.desc[descs[0]].next = to_leu16(descs[1]);

	/* subsequent descriptors: data from mblk chain */
	i = 1;
	for (m = mp; m != NULL; m = m->cont) {
		size_t seg_len = m->wptr - m->rptr;
		volatile struct vring_desc *desc;
		paddr_t paddr;

		if (seg_len == 0)
			continue;

		desc = &m_tx_vq.desc[descs[i]];

		if(mp->wptr <= m->rptr) {
			DKDevLog(self, "bad mblk (wptr <= rptr)\n");
			ke_spinlock_exit(&m_tx_vq.spinlock, ipl);
			return -EINVAL;
		}

		kassert((uintptr_t)m->rptr / PGSIZE ==
		    (uintptr_t)(m->rptr + seg_len - 1) / PGSIZE);

		if ((uintptr_t)m->rptr >= HHDM_BASE &&
		    (uintptr_t)m->rptr < HHDM_BASE + HHDM_SIZE) {
			paddr = v2p((vaddr_t)m->rptr);
		} else if ((uintptr_t)m->rptr >= PIN_HEAP_BASE &&
		    (uintptr_t)m->rptr < PIN_HEAP_BASE + PIN_HEAP_SIZE) {
			paddr = vm_translate((vaddr_t)m->rptr);
		} else {
			kfatal("TX buffer not in pinned heap nor HHDM\n");
		}

		desc->addr = to_leu64(paddr);
		desc->len = to_leu32(seg_len);

		if (i < nsegs) {
			/* further segments follow */
			desc->flags = to_leu16(VRING_DESC_F_NEXT);
			desc->next = to_leu16(descs[i + 1]);
		} else {
			/* final segment */
			desc->flags = to_leu16(0);
		}

		i++;
	}

	TAILQ_INSERT_TAIL(&m_tx_inflight_reqs, req, queue_entry);

	[m_transport submitDescNum:descs[0] toQueue:&m_tx_vq];
	[m_transport notifyQueue:&m_tx_vq];

	ke_spinlock_exit(&m_tx_vq.spinlock, ipl);

	return 0;
}

- (void)processUsedDescriptor:(volatile struct vring_used_elem *)e
		      onQueue:(struct virtio_queue *)queue
{
	uint16_t desc_id = le32_to_native(e->id);
	uint32_t len = le32_to_native(e->len);

	if (queue == &m_rx_vq) {
		/* RX completion */
		struct vionic_rx_req *req;

		TAILQ_FOREACH(req, &m_rx_inflight_reqs, queue_entry) {
			if (req->first_desc_id == desc_id)
				break;
		}

		if (req == NULL) {
			DKDevLog(self, "RX completion for unknown desc %u\n",
			    desc_id);
			return;
		}

		TAILQ_REMOVE(&m_rx_inflight_reqs, req, queue_entry);

		/* free the descriptor */
		[m_transport freeDescNum:desc_id onQueue:&m_rx_vq];

		if (len < sizeof(struct virtio_net_hdr_v1))
			kdprintf("virtio-nic: empty RX packet?\n");
		else
			[self handleReceivedPacket:req->buffer +
			    sizeof(struct virtio_net_hdr_v1)
					    length:len -
			    sizeof(struct virtio_net_hdr_v1)];

		/* buffer can go back to freelist */
		TAILQ_INSERT_TAIL(&m_rx_free_reqs, req, queue_entry);

	} else if (queue == &m_tx_vq) {
		/* TX completion */
		struct vionic_tx_req *req;
		uint16_t next_desc;

		TAILQ_FOREACH(req, &m_tx_inflight_reqs, queue_entry) {
			if (req->first_desc_id == desc_id)
				break;
		}

		if (req == NULL) {
			DKDevLog(self, "TX completion for unknown desc %u\n",
			    desc_id);
			return;
		}

		TAILQ_REMOVE(&m_tx_inflight_reqs, req, queue_entry);

		/* free the descriptor chain */
		next_desc = desc_id;
		for (size_t i = 0; i < req->ndescs; i++) {
			volatile struct vring_desc *desc =
			    &m_tx_vq.desc[next_desc];
			uint16_t cur = next_desc;

			if (from_leu16(desc->flags) & VRING_DESC_F_NEXT)
				next_desc = from_leu16(desc->next);

			[m_transport freeDescNum:cur onQueue:&m_tx_vq];
		}

#if 0
		kprintf("virtio-nic: TX done, freed %u descriptors\n",
		    req->ndescs);
#endif

		/* return request to freelist */
		TAILQ_INSERT_TAIL(&m_tx_free_reqs, req, queue_entry);
	}
}

- (void)additionalDeferredProcessingForQueue:(virtio_queue_t *)queue
{
	if (queue == &m_rx_vq)
		[self replenishRxQueue]; /* replenish rx buffers now */
	else
		; /* nothing to do */
}

@end
