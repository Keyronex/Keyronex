/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Sun May 12 2024.
 */

#include <ddk/DKUtilities.h>
#include <ddk/DKVirtIOTransport.h>
#include <ddk/safe_endian.h>
#include <ddk/virtioreg.h>
#include <kdk/endian.h>
#include <kdk/kmem.h>
#include <kdk/object.h>
#include <kdk/queue.h>
#include <string.h>

#include "VirtIO9pPort.h"
#include "fs/9p/9p_buf.h"
#include "fs/9p/9pfs.h"

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

TAILQ_TYPE_HEAD(, VirtIO9pPort) tag_list = TAILQ_HEAD_INITIALIZER(tag_list);
static int counter = 0;

@implementation VirtIO9pPort

+ (VirtIO9pPort *)forTag:(const char *)tag
{
	VirtIO9pPort *port;
	TAILQ_FOREACH (port, &tag_list, m_tagListEntry) {
		if(strcmp(port->m_tagName, tag) == 0)
			return port;
	}
	return NULL;
}

- (instancetype)initWithTransport:(DKVirtIOTransport *)transport
{
	volatile struct virtio_9p_config *cfg;
	size_t tag_len;
	int r;

	self = [super init];
	m_transport = transport;

	cfg = m_transport.deviceConfig;

	kmem_asprintf(&m_name, "virtio-9p-port");
	TAILQ_INIT(&in_flight_reqs);

	[m_transport resetDevice];

	if (![m_transport exchangeFeaturesMandatory:VIRTIO_F_VERSION_1 optional:NULL]) {
		DKDevLog(self, "Featuure exchange failed\n");
		return nil;
	}

	r = [m_transport setupQueue:&m_reqQueue index:0];
	if (r != 0) {
		DKDevLog(self, "Failed to setup command queue: %d", r);
		return nil;
	}

	r = [m_transport enableDevice];
	if (r != 0) {
		DKDevLog(self, "Failed to enable device: %d\n", r);
		return nil;
	}

	TAILQ_INIT(&free_reqs);
	TAILQ_INIT(&in_flight_reqs);
	TAILQ_INIT(&pending_packets);

	size_t nrequests = m_reqQueue.length / 2;
	m_requests = (void *)vm_kalloc(1, 0);
	for (int i = 0; i < nrequests; i++)
		TAILQ_INSERT_TAIL(&free_reqs, &m_requests[i], queue_entry);

	tag_len = MIN2(from_leu16(cfg->tag_len), 63);

	memcpy(m_tagName, (const void *)cfg->tag, tag_len);
	m_tagName[tag_len] = '\0';
	TAILQ_INSERT_TAIL(&tag_list, self, m_tagListEntry);

	DKDevLog(self, "Tag: %s\n", m_tagName);

	return self;
}

- (void)submitRequest:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_current(iop);
	uint16_t descs[10];
	size_t ndescs = 2;
	size_t di = 0; /* desc iterator */
	struct ninep_buf *in_buf = frame->ninep.ninep_in;
	struct ninep_buf *out_buf = frame->ninep.ninep_out;
	vm_mdl_t *mdl = frame->mdl;
	struct vio9p_req *req = TAILQ_FIRST(&free_reqs);

	kassert(req != NULL);
	TAILQ_REMOVE(&in_flight_reqs, req, queue_entry);

	if (mdl != NULL)
		ndescs += mdl->nentries;

	for (int i = 0; i < ndescs; i++)
		descs[i] = [m_transport allocateDescNumOnQueue:&m_reqQueue];

	req->ndescs = ndescs;
	req->first_desc_id = descs[0];
	req->iop = iop;

	/* the in buffer */
	m_reqQueue.desc[descs[0]].len = in_buf->data->size;
	m_reqQueue.desc[descs[0]].addr = to_leu64(V2P(in_buf->data));
	m_reqQueue.desc[descs[0]].flags = to_leu16(VRING_DESC_F_NEXT);
	m_reqQueue.desc[descs[0]].next = to_leu16(descs[1]);
	di++;

	/* the in-mdl, if extant */
	if (mdl && !mdl->write) {
		for (size_t i = 0; i < mdl->nentries; i++) {
			paddr_t paddr = vm_mdl_paddr(mdl, i * PGSIZE);
			m_reqQueue.desc[descs[di]].addr = to_leu64(paddr);
			m_reqQueue.desc[descs[di]].len = to_leu32(PGSIZE);
			m_reqQueue.desc[descs[di]].flags = to_leu16(
			    VRING_DESC_F_NEXT);
			m_reqQueue.desc[descs[di]].next = to_leu16(
			    descs[di + 1]);
			di++;
		}
	}

	/* the out header */
	m_reqQueue.desc[descs[di]].len = to_leu32(out_buf->bufsize);
	m_reqQueue.desc[descs[di]].addr = to_leu64(V2P(out_buf->data));
	m_reqQueue.desc[descs[di]].flags = to_leu16(VRING_DESC_F_WRITE);

	if (mdl && mdl->write) {
		m_reqQueue.desc[descs[di]].flags.value |= to_leu16(
		    VRING_DESC_F_NEXT).value;
		m_reqQueue.desc[descs[di]].next = to_leu16(descs[di + 1]);
		di++;

		for (size_t i = 0; i < mdl->nentries; i++) {
			paddr_t paddr = vm_mdl_paddr(mdl, i * PGSIZE);
			m_reqQueue.desc[descs[di]].addr = to_leu64(paddr);
			m_reqQueue.desc[descs[di]].len = to_leu32(PGSIZE);
			if (i == mdl->nentries - 1) {
				m_reqQueue.desc[descs[di]].flags =
				    to_leu16(VRING_DESC_F_WRITE);
				break;
			}
			m_reqQueue.desc[descs[di]].flags = to_leu16(
			    VRING_DESC_F_WRITE | VRING_DESC_F_NEXT);
			m_reqQueue.desc[descs[di]].next = to_leu16(
			    descs[di + 1]);
			di++;
		}
	}

	TAILQ_INSERT_HEAD(&in_flight_reqs, req, queue_entry);

	[m_transport submitDescNum:descs[0] toQueue:&m_reqQueue];
	[m_transport notifyQueue:&m_reqQueue];
}

- (void)deferredProcessing
{
#if 0
	ipl_t ipl = ke_spinlock_acquire(&m_reqQueue.spinlock);
	processVirtQueue(&m_reqQueue, self);
	while (true) {
		iop_t *iop;
		iop_frame_t *frame;
		size_t ndescs;

		/* if not even 2 descs available, can't do anything yet */
		if (m_reqQueue.nfree_descs < 2)
			return;

		iop = TAILQ_FIRST(&pending_packets);
		if (!iop)
			break;

		TAILQ_REMOVE(&pending_packets, iop, dev_queue_entry);

		frame = iop_stack_current(iop);

		ndescs = 2;
		if (frame->mdl != NULL) {
			kassert(frame->mdl->nentries <= 8);
			ndescs += frame->mdl->nentries;
		}

		if (ndescs <= m_reqQueue.nfree_descs)
			[self submitRequest:iop];
		else
			break;
	}
	ke_spinlock_release(&m_reqQueue.spinlock, ipl);
#endif
	kfatal("implement me\n");
}

- (void)processUsedDescriptor:(volatile struct vring_used_elem *)e
		      onQueue:(struct virtio_queue *)queue
{
	struct vio9p_req *req;
	iop_t *iop;
	uint16_t descidx = le32_to_native(e->id);
	size_t ndescs = 0;

	TAILQ_FOREACH (req, &in_flight_reqs, queue_entry) {
		if (req->first_desc_id == le32_to_native(e->id))
			break;
	}

	if (req == NULL) {
		kprintf("vio9p completion without a request: desc id is %u\n",
		    le32_to_native(e->id));
		TAILQ_FOREACH (req, &in_flight_reqs, queue_entry)
			kprintf(" - in-flight req, first desc is %u\n",
			    req->first_desc_id);
		kfatal("giving up.\n");
	}

	TAILQ_REMOVE(&in_flight_reqs, req, queue_entry);
	while (true) {
		volatile struct vring_desc *desc = &m_reqQueue.desc[descidx];

		if (!(from_leu16(desc->flags) & VRING_DESC_F_NEXT)) {
			[m_transport freeDescNum:descidx onQueue:&m_reqQueue];
			ndescs++;
			break;
		} else {
			uint16_t oldidx = descidx;
			descidx = from_leu16(desc->next);
			[m_transport freeDescNum:oldidx onQueue:&m_reqQueue];
			ndescs++;
		}
	}

	kassert(ndescs == req->ndescs);

	iop = req->iop;
	TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);

	/* this might be better in a separate DPC, or link iop to a list */
	iop_continue(iop, kIOPRetCompleted);
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	ipl_t ipl;
	iop_frame_t *frame = iop_stack_current(iop);

	kassert(frame->function == kIOPType9p);
	ipl = ke_spinlock_acquire(&m_reqQueue.spinlock);
	TAILQ_INSERT_TAIL(&pending_packets, iop, dev_qlink);
	ke_spinlock_release(&m_reqQueue.spinlock, ipl);
	//[m_transport enqueueDPC];

	return kIOPRetPending;
}


@end
