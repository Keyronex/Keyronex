#include <kern/kmem.h>
#include <vm/vm.h>

#include "VirtIOBlockDevice.h"
#include "virtio_blk.h"

@implementation VirtIOBlockDevice

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

	[self enableDevice];

	uint32_t virtq_desc[3];
	for (int i = 0; i < 3; i++) {
		virtq_desc[i] = i; //[self allocateDesc];
	}

	struct virtio_blk_outhdr hdr;
	hdr.sector = 0;
	hdr.type = VIRTIO_BLK_T_IN;

	vm_page_t *dataPage = vm_pagealloc(true, &vm_pgwiredq);

	queue.desc[virtq_desc[0]].len = sizeof(struct virtio_blk_outhdr);
	queue.desc[virtq_desc[0]].addr = (uint64_t)V2P(&hdr);
	queue.desc[virtq_desc[0]].flags = VRING_DESC_F_NEXT;
	queue.desc[virtq_desc[0]].next = virtq_desc[1];

	queue.desc[virtq_desc[1]].len = 512;
	queue.desc[virtq_desc[1]].addr = dataPage->paddr;
	queue.desc[virtq_desc[1]].flags = VRING_DESC_F_NEXT |
	    VRING_DESC_F_WRITE;
	queue.desc[virtq_desc[1]].next = virtq_desc[2];

	uint8_t endhdr = 0;

	queue.desc[virtq_desc[2]].len = 1;
	queue.desc[virtq_desc[2]].addr = (uint64_t)V2P(&endhdr);
	queue.desc[virtq_desc[2]].flags = VRING_DESC_F_WRITE;

	queue.avail->ring[0] = virtq_desc[0];
	__sync_synchronize();
	queue.avail->idx += 1;
	__sync_synchronize();

	[self notifyQueue:&queue];

	return self;
}

@end
