#ifndef VIRTIOBLOCKDEVICE_H_
#define VIRTIOBLOCKDEVICE_H_

#include <dev/VirtIOFamily/VirtIODevice.h>

#include <devicekit/DKDisk.h>

@interface VirtIOBlockDevice : VirtIODevice {
	struct virtio_blk_config *cfg;
	dk_virtio_queue_t	  queue;

	/* protected by queue->spinlock */
	TAILQ_HEAD(, vioblk_request) free_reqs;
	/* protected by queue->spinlock */
	TAILQ_HEAD(, vioblk_request) in_flight_reqs;
}

- initWithVirtIOInfo:(struct dk_virtio_info *)info;

- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)queue;

@end

#endif /* VIRTIOBLOCKDEVICE_H_ */
