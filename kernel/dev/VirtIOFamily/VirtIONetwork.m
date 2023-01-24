#include <sys/types.h>

#include <kern/kmem.h>
#include <vm/vm.h>

#include <nanokern/thread.h>

#include "VirtIONetwork.h"


@implementation VirtIONetwork

- initWithVirtIOInfo:(struct dk_virtio_info *)vioInfo
{
	self = [super initWithVirtIOInfo:vioInfo];

	kmem_asprintf(&m_name, "VirtIONetwork0");

	/* TODO(med): for non-qemu we might check VIRTIO_BLK_F_SIZE_MAX too */
	if (![self exchangeFeatures:0]) {
		DKDevLog(self, "Feature exchange failed.");
		[self dealloc];
		return nil;
	}

	[self enableDevice];

	[self registerDevice];

	return self;
}


- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)aQueue
{
	fatal("not handled yet\n");
}

@end
