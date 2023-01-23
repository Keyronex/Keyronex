#ifndef VIRTIOBLOCKDEVICE_H_
#define VIRTIOBLOCKDEVICE_H_

#include <dev/VirtIODevice.h>

@interface VirtIOBlockDevice : VirtIODevice
{
	struct virtio_blk_config *cfg;
	dk_virtio_queue_t queue;
}

- initWithVirtIOInfo:(struct dk_virtio_info *)info;

@end

#endif /* VIRTIOBLOCKDEVICE_H_ */
