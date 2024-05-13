#include <string.h>

#include "VirtIO9pPort.h"
#include "ddk/virtioreg.h"
#include "dev/safe_endian.h"
#include "fs/9p/9p_buf.h"
#include "fs/9p/9pfs.h"
#include "kdk/dev.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

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

@interface VirtIO9pPort (Private)
- (void)request:(int)reqType
	 blocks:(io_blksize_t)blocks
       atOffset:(io_blkoff_t)offset
	withMDL:(vm_mdl_t *)mdl
	    iop:(iop_t *)iop;
@end

void processVirtQueue(struct virtio_queue *queue, id delegate);

#define PROVIDER ((DKDevice<DKVirtIOTransport> *)m_provider)

static int counter = 0;

@implementation VirtIO9pPort

+ (BOOL)probeWithProvider:(DKDevice<DKVirtIOTransport> *)provider
{
	[[self alloc] initWithProvider:provider];
	return YES;
}

- (instancetype)initWithProvider:(DKDevice<DKVirtIOTransport> *)provider
{
	int r;
	volatile struct virtio_9p_config *cfg;
	char tagname[64] = { 0 };

	self = [super initWithProvider:provider];

	cfg = provider.deviceConfig;

	kmem_asprintf(obj_name_ptr(self), "virtio-9p-%u", counter++);
	TAILQ_INIT(&in_flight_reqs);

	provider.delegate = self;
	[provider resetDevice];

	if (![provider exchangeFeatures:VIRTIO_F_VERSION_1]) {
		DKDevLog(self, "Featuure exchange failed\n");
		return nil;
	}

	r = [provider setupQueue:&m_reqQueue index:0];
	if (r != 0) {
		DKDevLog(self, "Failed to setup command queue: %d", r);
		return nil;
	}

	r = [provider enableDevice];
	if (r != 0) {
		DKDevLog(self, "Failed to enable device: %d", r);
	}

	TAILQ_INIT(&free_reqs);
	TAILQ_INIT(&in_flight_reqs);
	TAILQ_INIT(&pending_packets);

	size_t nrequests = m_reqQueue.length / 2;
	m_requests = (void *)vm_kalloc(1, 0);
	for (int i = 0; i < nrequests; i++)
		TAILQ_INSERT_TAIL(&free_reqs, &m_requests[i], queue_entry);

	[self registerDevice];
	memcpy(tagname, (const void *)cfg->tag,
	    MIN2(from_leu16(cfg->tag_len), 63));
	DKLogAttachExtra(self, "Tag: %s\n", tagname);

	[NinepFS probeWithProvider:self];

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

	if (mdl != NULL)
		ndescs += mdl->nentries;

	for (int i = 0; i < ndescs; i++)
		descs[i] = [PROVIDER allocateDescNumOnQueue:&m_reqQueue];

	req->ndescs = ndescs;
	req->first_desc_id = descs[0];
	req->iop = iop;

	/* the in buffer */
	m_reqQueue.desc[descs[0]].len = in_buf->data->size.value;
	m_reqQueue.desc[descs[0]].addr = native_to_le64(V2P(in_buf->data));
	m_reqQueue.desc[descs[0]].flags = native_to_le16(VRING_DESC_F_NEXT);
	m_reqQueue.desc[descs[0]].next = native_to_le16(descs[1]);
	di++;

	/* the in-mdl, if extant */
	if (mdl && !mdl->write) {
		for (size_t i = 0; i < mdl->nentries; i++) {
			paddr_t paddr = vm_mdl_paddr(mdl, i * PGSIZE);
			m_reqQueue.desc[descs[di]].addr = native_to_le64(paddr);
			m_reqQueue.desc[descs[di]].len = native_to_le32(PGSIZE);
			m_reqQueue.desc[descs[di]].flags = native_to_le16(
			    VRING_DESC_F_NEXT);
			m_reqQueue.desc[descs[di]].next = native_to_le16(
			    descs[di + 1]);
			di++;
		}
	}

	/* the out header */
	m_reqQueue.desc[descs[di]].len = native_to_le32(out_buf->bufsize);
	m_reqQueue.desc[descs[di]].addr = native_to_le64(V2P(out_buf->data));
	m_reqQueue.desc[descs[di]].flags = native_to_le16(VRING_DESC_F_WRITE);

	if (mdl && mdl->write) {
		m_reqQueue.desc[descs[di]].flags |= native_to_le16(
		    VRING_DESC_F_NEXT);
		m_reqQueue.desc[descs[di]].next = native_to_le16(descs[di + 1]);
		di++;

		for (size_t i = 0; i < mdl->nentries; i++) {
			paddr_t paddr = vm_mdl_paddr(mdl, i * PGSIZE);
			m_reqQueue.desc[descs[di]].addr = native_to_le64(paddr);
			m_reqQueue.desc[descs[di]].len = native_to_le32(PGSIZE);
			if (i == mdl->nentries - 1)
				break;
			m_reqQueue.desc[descs[di]].flags = native_to_le16(
			    VRING_DESC_F_WRITE | VRING_DESC_F_NEXT);
			m_reqQueue.desc[descs[di]].next = native_to_le16(
			    descs[di + 1]);
			di++;
		}
	}

	TAILQ_INSERT_HEAD(&in_flight_reqs, req, queue_entry);

	[PROVIDER submitDescNum:descs[0] toQueue:&m_reqQueue];
	[PROVIDER notifyQueue:&m_reqQueue];
}

- (void)deferredProcessing
{
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

		if (!(le16_to_native(desc->flags) & VRING_DESC_F_NEXT)) {
			[PROVIDER freeDescNum:descidx onQueue:&m_reqQueue];
			ndescs++;
			break;
		} else {
			uint16_t oldidx = descidx;
			descidx = le16_to_native(desc->next);
			[PROVIDER freeDescNum:oldidx onQueue:&m_reqQueue];
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
	iop_frame_t *frame = iop_stack_current(iop);

	kassert(frame->function == kIOPType9p);
	/*! (!) needs to be guarded by spinlock.... */
	TAILQ_INSERT_TAIL(&pending_packets, iop, dev_queue_entry);
	[PROVIDER enqueueDPC];

	return kIOPRetPending;
}

@end
