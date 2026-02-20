/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file VirtIO9pPort.m
 * @brief VirtIO 9p port.
 */

#include <sys/vnode.h>
#include <sys/kmem.h>

#include <libkern/lib.h>

#include <devicekit/virtio/DKVirtIOTransport.h>
#include <devicekit/virtio/VirtIO9pPort.h>
#include <devicekit/virtio/virtioreg.h>

#include <fs/9p/9pbuf.h>
#include <fs/devfs/devfs.h>

struct virtio_9p_config {
	/* length of the tag name */
	leu16_t tag_len;
	/* non-NULL terminated tag name */
	uint8_t tag[];
} __attribute__((packed));

struct vio9p_req {
	/*! linkage in inflight or free reqs list */
	TAILQ_ENTRY(vio9p_req) queue_entry;
	/*! generic 9p request structure */
	iop_t *iop;
	/*! number of descriptors we calculated we'd need */
	uint16_t ndescs;
	/*! the first desc, by that this request is identified */
	uint16_t first_desc_id;
};

static dev_class_t ninep_dev_class;
static dev_ops_t ninep_dev_ops;

@implementation VirtIO9pPort

#define DKDevLog(dev, fmt, ...) \
    kdprintf("virtio9p: " fmt, ##__VA_ARGS__)

#define m_cfg	    ((volatile struct virtio_9p_config *)m_transport.deviceConfig)

- (instancetype)initWithTransport:(DKVirtIOTransport*) transport
{
	volatile struct virtio_9p_config *cfg;
	size_t tag_len;

	self = [super init];
	m_transport = transport;
	cfg = m_transport.deviceConfig;

	[m_transport resetDevice];

	if (![m_transport exchangeFeaturesMandatory:VIRTIO_F_VERSION_1 optional:NULL]) {
		DKDevLog(self, "Feature exchange failed\n");
		return nil;
	}

	[m_transport setupQueue:&m_io_vq index:0];
	[m_transport enableDevice];

	TAILQ_INIT(&m_free_reqs);
	TAILQ_INIT(&m_inflight_reqs);
	TAILQ_INIT(&m_iop_q);

	m_requests_n = m_io_vq.length / 2;
	kassert(m_requests_n * sizeof(struct vio9p_req) <= PGSIZE);
	m_reqs = kmem_alloc(PGSIZE);
	for (int i = 0; i < m_requests_n; i++)
		TAILQ_INSERT_TAIL(&m_free_reqs, &m_reqs[i], queue_entry);

	tag_len = MIN2(from_leu16(cfg->tag_len), 63);
	m_tag = kmem_alloc(tag_len + 1);
	memcpy(m_tag, (const void *)cfg->tag, tag_len);
	m_tag[tag_len] = '\0';

	DKDevLog(self, "Tag: %s\n", m_tag);

	devfs_create_node(&ninep_dev_class, self, "vio9p:%s", m_tag);

	return self;
}

/* todo: this doesn't deal with sglists as it ought to */
- (void)submitRequest:(iop_t *)iop
{
	iop_frame_t *frame = iop_current_frame(iop);
	uint16_t descs[18];
	size_t ndescs = 2;
	size_t di = 0; /* desc iterator */
	struct ninep_buf *in_buf = frame->ninep.ninep_in;
	struct ninep_buf *out_buf = frame->ninep.ninep_out;
	sg_list_t *mdl = frame->sglist;
	struct vio9p_req *req = TAILQ_FIRST(&m_free_reqs);

	kassert(req != NULL);
	TAILQ_REMOVE(&m_inflight_reqs, req, queue_entry);

	if (mdl != NULL)
		frame->sglist_offset = 0;

	if (mdl != NULL)
		ndescs += sglist_breaks(mdl, frame->sglist_offset,
		    sglist_size(mdl));

	for (int i = 0; i < ndescs; i++)
		descs[i] = [m_transport allocateDescNumOnQueue:&m_io_vq];

	req->ndescs = ndescs;
	req->first_desc_id = descs[0];
	req->iop = iop;

	/* the in buffer */
	m_io_vq.desc[descs[0]].len = in_buf->data->size;
	m_io_vq.desc[descs[0]].addr = to_leu64(v2p((vaddr_t)in_buf->data));
	m_io_vq.desc[descs[0]].flags = to_leu16(VRING_DESC_F_NEXT);
	m_io_vq.desc[descs[0]].next = to_leu16(descs[1]);
	di++;

	/* the in-mdl, if extant */
	if (mdl && !frame->sglist_write) {
		for (size_t i = 0; i < mdl->elems_n; i++) {
			paddr_t paddr = sglist_paddr(mdl, i * PGSIZE, NULL);
			m_io_vq .desc[descs[di]].addr = to_leu64(paddr);
			m_io_vq .desc[descs[di]].len = to_leu32(PGSIZE);
			m_io_vq .desc[descs[di]].flags =
			    to_leu16(VRING_DESC_F_NEXT);
			m_io_vq .desc[descs[di]].next =
			    to_leu16(descs[di + 1]);
			di++;
		}
	}

	/* the out header */
	m_io_vq.desc[descs[di]].len = to_leu32(out_buf->bufsize);
	m_io_vq.desc[descs[di]].addr = to_leu64(v2p((vaddr_t)out_buf->data));
	m_io_vq.desc[descs[di]].flags = to_leu16(VRING_DESC_F_WRITE);

	if (mdl && frame->sglist_write) {
		m_io_vq.desc[descs[di]].flags.value |= to_leu16(
		    VRING_DESC_F_NEXT).value;
		m_io_vq.desc[descs[di]].next = to_leu16(descs[di + 1]);
		di++;

		for (size_t i = 0; i < mdl->elems_n; i++) {
			paddr_t paddr = sglist_paddr(mdl, i * PGSIZE, NULL);
			m_io_vq.desc[descs[di]].addr = to_leu64(paddr);
			m_io_vq.desc[descs[di]].len = to_leu32(PGSIZE);
			if (i == mdl->elems_n - 1) {
				m_io_vq.desc[descs[di]].flags =
				    to_leu16(VRING_DESC_F_WRITE);
				break;
			}
			/* otherwise */
			m_io_vq.desc[descs[di]].flags =
			    to_leu16(VRING_DESC_F_WRITE | VRING_DESC_F_NEXT);
			m_io_vq.desc[descs[di]].next =
			    to_leu16(descs[di + 1]);
			di++;
		}
	}

	TAILQ_INSERT_HEAD(&m_inflight_reqs, req, queue_entry);

	[m_transport submitDescNum:descs[0] toQueue:&m_io_vq ];
	[m_transport notifyQueue:&m_io_vq ];
}


- (void)additionalDeferredProcessingForQueue:(virtio_queue_t *)queue
{
	kassert(queue == &m_io_vq);

	while (true) {
		iop_t *iop;
		iop_frame_t *frame;
		size_t ndescs;

		/* if not even 2 descs available, can't do anything yet */
		if (m_io_vq.nfree_descs < 2)
			return;

		iop = TAILQ_FIRST(&m_iop_q);
		if (!iop)
			break;

		TAILQ_REMOVE(&m_iop_q, iop, dev_qlink);

		frame = iop_current_frame(iop);

		ndescs = 2;
		if (frame->sglist != NULL) {
			size_t breaks = sglist_breaks(frame->sglist,
			    frame->sglist_offset, sglist_size(frame->sglist));
			kassert(breaks <= 16);
			ndescs += breaks;
		}

		if (ndescs <= m_io_vq.nfree_descs)
			[self submitRequest:iop];
		else
			break;
	}
}

- (void)processUsedDescriptor:(volatile struct vring_used_elem *)e
		      onQueue:(struct virtio_queue *)queue
{
	struct vio9p_req *req;
	iop_t *iop;
	uint16_t descidx = le32_to_native(e->id);
	size_t ndescs = 0;

	TAILQ_FOREACH(req, &m_inflight_reqs, queue_entry) {
		if (req->first_desc_id == le32_to_native(e->id))
			break;
	}

	if (req == NULL) {
		kdprintf("vio9p completion without a request: desc id is %u\n",
		    le32_to_native(e->id));
		TAILQ_FOREACH(req, &m_inflight_reqs, queue_entry)
			kdprintf(" - in-flight req, first desc is %u\n",
			    req->first_desc_id);
		kfatal("giving up.\n");
	}

	TAILQ_REMOVE(&m_inflight_reqs, req, queue_entry);
	while (true) {
		volatile struct vring_desc *desc = &m_io_vq.desc[descidx];

		if (!(from_leu16(desc->flags) & VRING_DESC_F_NEXT)) {
			[m_transport freeDescNum:descidx onQueue:&m_io_vq];
			ndescs++;
			break;
		} else {
			uint16_t oldidx = descidx;
			descidx = from_leu16(desc->next);
			[m_transport freeDescNum:oldidx onQueue:&m_io_vq];
			ndescs++;
		}
	}

	kassert(ndescs == req->ndescs);

	iop = req->iop;
	TAILQ_INSERT_TAIL(&m_free_reqs, req, queue_entry);

	/* this might be better in a separate DPC, or link iop to a list */
	iop_continue(iop, kIOPRetCompleted);
}


- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	ipl_t ipl;
	iop_frame_t *frame = iop_current_frame(iop);

	kassert(frame->op == kIOP9p);
	ipl = ke_spinlock_enter(&m_io_vq.spinlock);
	TAILQ_INSERT_TAIL(&m_iop_q, iop, dev_qlink);
	[self additionalDeferredProcessingForQueue:&m_io_vq];
	ke_spinlock_exit(&m_io_vq.spinlock, ipl);

	return kIOPRetPending;
}

@end

static iop_return_t
iop_dispatch(void *devprivate, iop_t *iop)
{
	VirtIO9pPort *port = (VirtIO9pPort *)devprivate;
	return [port dispatchIOP:iop];
}

static dev_ops_t ninep_dev_ops = {
	.stack_depth = 1,
	.iop_dispatch = iop_dispatch,
};

static dev_class_t ninep_dev_class = {
	.kind = DEV_KIND_CHAR,
	.charops = &ninep_dev_ops,
};
