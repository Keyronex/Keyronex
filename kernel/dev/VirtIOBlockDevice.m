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
	for (int i = 0; i < 2; i++) {
		virtq_desc[i] = [self allocateDesc];
	}

	struct virtio_blk_outhdr hdr;
	hdr.sector = 0;
	hdr.type = VIRTIO_BLK_T_OUT;

	queue.desc[virtq_desc[0]].len = sizeof(struct virtio_blk_outhdr);
	queue.desc[virtq_desc[0]].addr = (uint64_t)V2P(&hdr);
	queue.desc[virtq_desc[0]].flags = 0;

	return self;
}

@end
