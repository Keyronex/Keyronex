
#ifndef VIRTIONETWORK_H_
#define VIRTIONETWORK_H_

#include <dev/VirtIOFamily/VirtIODevice.h>

#include <devicekit/DKDisk.h>

@interface VirtIONetwork : VirtIODevice {
}

- initWithVirtIOInfo:(struct dk_virtio_info *)info;


@end

#endif /* VIRTIONETWORK_H_ */
