#include <sys/types.h>

#include <kern/kmem.h>
#include <nanokern/queue.h>
#include <nanokern/thread.h>
#include <vm/vm.h>

#include "VirtIOBlockDevice.h"
#include "virtio_blk.h"
#include "virtioreg.h"

struct vioblk_request {
	/* linkage in in_flight_reqs or free_reqs */
	TAILQ_ENTRY(vioblk_request) queue_entry;
	/* first descriptor */
	struct virtio_blk_outhdr hdr;
	/* final descriptor */
	uint8_t flags;
	/*! the first desc, by that this request is identified */
	uint16_t first_desc_id;
	/*! completion callback */
	struct dk_diskio_completion *completion;
};

ksemaphore_t sem;

@implementation VirtIOBlockDevice

static void
done_io(void *arg, ssize_t len)
{
	kprintf("done async I/O, signalling waiters\n");
	nk_semaphore_release(&sem, 1);
}

- initWithVirtIOInfo:(struct dk_virtio_info *)vioInfo
{
	self = [super initWithVirtIOInfo:vioInfo];

	kmem_asprintf(&m_name, "VirtIOBlockDevice0");

	/* TODO(med): for non-qemu we might check VIRTIO_BLK_F_SIZE_MAX too */
	if (![self exchangeFeatures:VIRTIO_BLK_F_SEG_MAX]) {
		DKDevLog(self, "Feature exchange failed.");
		[self dealloc];
		return nil;
	}

	cfg = info.device_cfg;
	kassert([self setupQueue:&queue index:0] == 0);
	info.queues = kmem_alloc(sizeof(dk_virtio_queue_t *));
	info.queues[0] = &queue;
	info.num_queues = 1;

	TAILQ_INIT(&free_reqs);
	TAILQ_INIT(&in_flight_reqs);

	/* minimum of 3 descriptors per request; allocate reqs accordingly */
	/* TODO(high): ugly! do it better */
	vm_page_t *page = vm_pagealloc(true, &vm_pgwiredq);
	vaddr_t	   addr = (vaddr_t)P2V(page->paddr);
	for (int i = 0; i < ROUNDUP(queue.length, 3) / 3; i++) {
		struct vioblk_request *req = (void *)(addr + sizeof(*req) * i);
		TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);
	}

	[self enableDevice];

#if 0
	struct dk_diskio_completion comp;
	comp.callback = done_io;

	nk_semaphore_init(&sem, 0);

	vm_page_t *dataPage = vm_pagealloc(true, &vm_pgwiredq);

	for (int i = 0; i < 2; i++) {
		[self commonRequest:VIRTIO_BLK_T_IN
			     blocks:1
				 at:i
			     buffer:dataPage->paddr
			 completion:&comp];

		nk_wait(&sem, "test_virtio", false, false, -1);

		char *res = P2V(dataPage->paddr);
		res[512] = '\0';
		DKDevLog(self, "Block %d contains: \"%s\"\n", i, res);
	}
#endif

	[self registerDevice];
	DKLogAttach(self);

	struct virtio_drive_attachment_info driveInfo;
	driveInfo.blocksize = 512;
	driveInfo.provider = self;
	driveInfo.maxBlockTransfer = PGSIZE / 512;
	driveInfo.nBblocks = cfg->capacity;
	driveInfo.controllerNum = 0;

	VirtIODrive *disk = [[VirtIODrive alloc] initWithInfo:&driveInfo];
	(void)disk;

	return self;
}

- (int)commonRequest:(int)kind
	      blocks:(blkcnt_t)nblocks
		  at:(blkoff_t)block
	      buffer:(paddr_t)buf
	  completion:(struct dk_diskio_completion *)completion
{
	uint32_t virtq_desc[3];
	ipl_t	 ipl;

	for (int i = 0; i < 3; i++) {
		virtq_desc[i] = i; //[self allocateDesc];
	}

	ipl = nk_spinlock_acquire_at(&queue.spinlock, kSPLBIO);

	struct vioblk_request *req = TAILQ_FIRST(&free_reqs);
	TAILQ_REMOVE(&free_reqs, req, queue_entry);

	req->hdr.sector = block;
	req->hdr.type = kind;
	req->completion = completion;
	req->first_desc_id = virtq_desc[0];

	queue.desc[virtq_desc[0]].len = sizeof(struct virtio_blk_outhdr);
	queue.desc[virtq_desc[0]].addr = (uint64_t)V2P(&req->hdr);
	queue.desc[virtq_desc[0]].flags = VRING_DESC_F_NEXT;
	queue.desc[virtq_desc[0]].next = virtq_desc[1];

	queue.desc[virtq_desc[1]].len = kind == VIRTIO_BLK_T_GET_ID ?
	    20 :
	    nblocks * 512;
	queue.desc[virtq_desc[1]].addr = buf;
	queue.desc[virtq_desc[1]].flags = VRING_DESC_F_NEXT |
	    VRING_DESC_F_WRITE;
	queue.desc[virtq_desc[1]].next = virtq_desc[2];

	queue.desc[virtq_desc[2]].len = 1;
	queue.desc[virtq_desc[2]].addr = (uint64_t)V2P(&req->flags);
	queue.desc[virtq_desc[2]].flags = VRING_DESC_F_WRITE;

	[self submitDescNum:virtq_desc[0] toQueue:&queue];

	TAILQ_INSERT_HEAD(&in_flight_reqs, req, queue_entry);

	[self notifyQueue:&queue];

	nk_spinlock_release(&queue.spinlock, ipl);

	return 0;
}

- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)aQueue
{
	struct vring_desc     *dout, *ddata;
	uint16_t	       ddataidx, dtailidx;
	struct vioblk_request *req;

	TAILQ_FOREACH (req, &in_flight_reqs, queue_entry) {
		if (req->first_desc_id == e->id)
			break;
	}

	if (!req || req->first_desc_id != e->id)
		kfatal("vioblk completion without a request\n");

	TAILQ_REMOVE(&in_flight_reqs, req, queue_entry);

	dout = &QUEUE_DESC_AT(&queue, e->id);
	kassert(dout->flags & VRING_DESC_F_NEXT);
	ddataidx = dout->next;
	ddata = &QUEUE_DESC_AT(&queue, ddataidx);
	kassert(ddata->flags & VRING_DESC_F_NEXT);
	dtailidx = ddata->next;
#if 0
	dtail = &QUEUE_DESC_AT(&queue, dtailidx);
#endif

	[self freeDescNum:e->id onQueue:&queue];
	[self freeDescNum:ddataidx onQueue:&queue];
	[self freeDescNum:dtailidx onQueue:&queue];

	if (req->flags & VIRTIO_BLK_S_IOERR) {
		DKDevLog(self, "I/O error\n");
		return;
	}

	req->completion->callback(req->completion->data, ddata->len);

	TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);
}

@end

#define DEVICE ((VirtIOBlockDevice *)provider)

@implementation VirtIODrive

- initWithInfo:(struct virtio_drive_attachment_info *)info;
{
	self = [super initWithProvider:info->provider];

	kmem_asprintf(&m_name, "VirtIODrive", info->controllerNum);
	m_nBlocks = info->nBblocks;
	m_blockSize = info->blocksize;
	m_maxBlockTransfer = info->maxBlockTransfer;
	[self registerDevice];

	DKLogAttachExtra(self, "%lu MiB (blocksize %ld, blocks %ld)\n",
	    m_nBlocks * m_blockSize / 1024 / 1024, m_blockSize, m_nBlocks);

	[[DKLogicalDisk alloc] initWithUnderlyingDisk:self
						 base:0
						 size:m_nBlocks * m_blockSize
						 name:[info->provider name]
					     location:0
					     provider:self];

	return self;
}

@end
