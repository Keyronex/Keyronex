#include <stdint.h>

#include "ObjFWRT.h"
#include "VirtIODisk.h"
#include "ddk/virtio_blk.h"
#include "ddk/virtioreg.h"
#include "dev/DOSFS.h"
#include "kdk/dev.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "dev/Ext2FS.h"

#define PROVIDER ((DKDevice<DKVirtIOTransport> *)m_provider)

struct vioblk_req {
	/* linkage in in_flight_reqs or free_reqs */
	TAILQ_ENTRY(vioblk_req) queue_entry;
	/* first descriptor */
	struct virtio_blk_outhdr hdr;
	/* final descriptor */
	uint8_t flags;
	/*! the first desc, by that this request is identified */
	uint16_t first_desc_id;
	/*! the IOP it belongs to */
	iop_t *iop;
};

static int counter = 0;

@interface VirtIODisk (Private)
- (void)request:(int)reqType
	   blocks:(io_blksize_t)blocks
	 atOffset:(io_blkoff_t)offset
	  withMDL:(vm_mdl_t *)mdl
	      iop:(iop_t *)iop;
@end

@implementation VirtIODisk

+ (BOOL)probeWithProvider:(DKDevice<DKVirtIOTransport> *)provider
{
	[[self alloc] initWithProvider:provider];
	return YES;
}

- (instancetype)initWithProvider:(DKDevice<DKVirtIOTransport> *)provider
{
	int r;
	volatile struct virtio_blk_config *cfg;

	self = [super initWithProvider:provider];

	cfg = provider.deviceConfig;

	kmem_asprintf(obj_name_ptr(self), "virtio-disk-%u", counter++);
	TAILQ_INIT(&in_flight_reqs);

	provider.delegate = self;
	[provider resetDevice];

	if (![provider exchangeFeatures:VIRTIO_F_VERSION_1]) {
		DKDevLog(self, "Featuure exchange failed\n");
		return nil;
	}

	r = [provider setupQueue:&m_ioQueue index:0];
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

	size_t nrequests = ROUNDUP(m_ioQueue.length, 3) / 3;
	m_requests = (void *)vm_kalloc(1, 0);
	for (int i = 0; i < nrequests; i++)
		TAILQ_INSERT_TAIL(&free_reqs, &m_requests[i], queue_entry);

	[self registerDevice];
	DKLogAttachExtra(self, "%" PRIu64 "KiB\n",
	    le64_to_native(cfg->capacity) * 512 / 1024);

	//vm_mdl_t *mdl = vm_mdl_buffer_alloc(1);
	//[self readWrite:NO blocks:8 atOffset:0 withMDL:mdl iop:NULL];
	//[Ext2FS probeWithVolume:self blockSize:512 blockCount:le64_to_native(cfg->capacity)];
	[DOSFS probeWithVolume:self blockSize:512 blockCount:le64_to_native(cfg->capacity)];
	return self;
}

- (void)request:(int)reqType
	 blocks:(io_blksize_t)blocks
       atOffset:(io_blkoff_t)offset
	withMDL:(vm_mdl_t *)mdl
	    iop:(iop_t *)iop
{
	uint32_t virtq_desc[7];
	size_t i = 0;
	size_t ndatadescs;

#if DEBUG_VIODISK == 1
	DKDevLog(self, "Strategy(%d, block %llx, blocks %llu)\n", reqType,
	    offset, blocks);
#endif

	ndatadescs = ROUNDUP(blocks * 512, PGSIZE) / PGSIZE;
	kassert(ndatadescs <= 4);

	for (unsigned i = 0; i < 2 + ndatadescs; i++)
		virtq_desc[i] = [PROVIDER allocateDescNumOnQueue:&m_ioQueue];

	struct vioblk_req *req = TAILQ_FIRST(&free_reqs);
	TAILQ_REMOVE(&free_reqs, req, queue_entry);

#if DEBUG_VIODISK == 1
	DKDevLog(this, "op type %d (nblocks %zu offset %d; req %p)\n", kind,
	    nblocks, block, req);
#endif

	req->hdr.sector = native_to_le64(offset);
	req->hdr.type = native_to_le32(reqType);
	req->first_desc_id = virtq_desc[0];
	req->iop = iop;

#define DEBUG_VIODISK 0

	m_ioQueue.desc[virtq_desc[i]].len = native_to_le32(
	    sizeof(struct virtio_blk_outhdr));
	m_ioQueue.desc[virtq_desc[i]].addr = native_to_le64(V2P(&req->hdr));
	m_ioQueue.desc[virtq_desc[i]].flags = native_to_le16(VRING_DESC_F_NEXT);
#if DEBUG_VIODISK == 1
	kprintf("NBlocks: %llu\n", blocks);
	kprintf("Initial Addr: 0x%zx\n", V2P(&req->hdr));
#endif
	m_ioQueue.desc[virtq_desc[i]].next = native_to_le16(virtq_desc[1]);

	size_t nbytes = blocks * 512;

	for (i = 1; i < 1 + ndatadescs; i++) {
		size_t len = MIN2(nbytes, PGSIZE);
		nbytes -= len;
		m_ioQueue.desc[virtq_desc[i]].len = native_to_le32(len);
		m_ioQueue.desc[virtq_desc[i]].addr = native_to_le64(
		    vm_mdl_paddr(mdl, (i - 1) * PGSIZE));

#if DEBUG_VIODISK == 1
		kprintf("Another Addr: 0x%zx/bytes: %zu/len %zu / given: %zu\n",
		    vm_mdl_paddr(mdl, (i - 1) * PGSIZE), nbytes, len,
		    m_ioQueue.desc[virtq_desc[i]].len);
#endif
		m_ioQueue.desc[virtq_desc[i]].flags = native_to_le16(
		    VRING_DESC_F_NEXT);
		if (reqType == VIRTIO_BLK_T_IN)
			m_ioQueue.desc[virtq_desc[i]].flags |= native_to_le16(
			    VRING_DESC_F_WRITE);
		m_ioQueue.desc[virtq_desc[i]].next = native_to_le16(
		    virtq_desc[i + 1]);
	}

	m_ioQueue.desc[virtq_desc[i]].len = native_to_le32(1);
	m_ioQueue.desc[virtq_desc[i]].addr = native_to_le64(V2P(&req->flags));

#if DEBUG_VIODISK == 1
	kprintf("Final Addr: 0x%zx\n", V2P(&req->flags));
#endif
	m_ioQueue.desc[virtq_desc[i]].flags = native_to_le16(
	    VRING_DESC_F_WRITE);

	TAILQ_INSERT_HEAD(&in_flight_reqs, req, queue_entry);

	[PROVIDER submitDescNum:virtq_desc[0] toQueue:&m_ioQueue];
	[PROVIDER notifyQueue:&m_ioQueue];
}

void processVirtQueue(struct virtio_queue *queue, id delegate);

- (void)deferredProcessing
{
	ipl_t ipl = ke_spinlock_acquire(&m_ioQueue.spinlock);
	processVirtQueue(&m_ioQueue, self);
	while (true) {
		iop_t *iop;
		iop_frame_t *frame;
		size_t ndescs;

		if (m_ioQueue.nfree_descs < 3)
			return;

		iop = TAILQ_FIRST(&pending_packets);
		if (!iop)
			break;

		TAILQ_REMOVE(&pending_packets, iop, dev_queue_entry);

		frame = iop_stack_current(iop);

		kassert(frame->function == kIOPTypeRead);
		kassert(
		    frame->rw.bytes <= PGSIZE || frame->rw.bytes % PGSIZE == 0);
		kassert(frame->rw.bytes < PGSIZE * 4);
		/* why? shouldn't it be / PGSIZE? */
		ndescs = 2 + frame->rw.bytes / 512;

		if (ndescs <= m_ioQueue.nfree_descs)
			[self request:VIRTIO_BLK_T_IN
			       blocks:frame->rw.bytes / 512
			     atOffset:frame->rw.offset / 512
			      withMDL:frame->mdl
				  iop:iop];
		else
			break;
	}
	ke_spinlock_release(&m_ioQueue.spinlock, ipl);
}

- (void)processUsedDescriptor:(volatile struct vring_used_elem *)e
		      onQueue:(struct virtio_queue *)queue

{
	volatile struct vring_desc *dout, *dnext;
	uint16_t dnextidx;
	struct vioblk_req *req;
	size_t bytes = 0;

	TAILQ_FOREACH (req, &in_flight_reqs, queue_entry) {
		if (req->first_desc_id == le32_to_native(e->id))
			break;
	}

	if (!req || req->first_desc_id != le32_to_native(e->id)) {
		kprintf("vioblk completion without a request: desc id is %u\n",
		    le32_to_native(e->id));
		TAILQ_FOREACH (req, &in_flight_reqs, queue_entry)
			kprintf(" - in-flight req, first desc is %u\n",
			    req->first_desc_id);
		kfatal("giving up.\n");
	}

	TAILQ_REMOVE(&in_flight_reqs, req, queue_entry);

	dout = &QUEUE_DESC_AT(&m_ioQueue, le32_to_native(e->id));
	kassert(le16_to_native(dout->flags) & VRING_DESC_F_NEXT);

	dnextidx = le16_to_native(dout->next);
	while (true) {
		dnext = &QUEUE_DESC_AT(&m_ioQueue, dnextidx);

		if (!(le16_to_native(dnext->flags) & VRING_DESC_F_NEXT)) {
			break;
		} else {
			uint16_t old_idx = dnextidx;
			bytes += le32_to_native(dnext->len);
			dnextidx = le16_to_native(dnext->next);
			[PROVIDER freeDescNum:old_idx onQueue:&m_ioQueue];
		}
	}


	[PROVIDER freeDescNum:le32_to_native(e->id) onQueue:&m_ioQueue];
	[PROVIDER freeDescNum:dnextidx onQueue:&m_ioQueue];

	if (req->flags & VIRTIO_BLK_S_IOERR ||
	    req->flags & VIRTIO_BLK_S_UNSUPP) {
		DKDevLog(self, "I/O error\n");
		for (;;)
			;
		return;
	}

#if DEBUG_VIODISK == 1
	DKDevLog(self, "done req %p yielding %zu bytes\n", req, bytes);
#endif
	/* this might be better in a separate DPC */
	iop_continue(req->iop, kIOPRetCompleted);

	TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);
}

- (iop_return_t)dispatchIOP: (iop_t*)iop
{
	iop_frame_t *frame = iop_stack_current(iop);

	kassert(frame->function == kIOPTypeRead);
	/*! (!) needs to be guarded by spinlock.... */
	TAILQ_INSERT_TAIL(&pending_packets, iop, dev_queue_entry);
	[PROVIDER enqueueDPC];

	return kIOPRetPending;
}

@end
