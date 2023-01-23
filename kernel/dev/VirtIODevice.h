#ifndef VIRTIODEVICE_H_
#define VIRTIODEVICE_H_

#include <dev/PCIBus.h>

struct dk_virtio_info {
	dk_device_pci_info_t	      pciInfo;
	struct virtio_pci_common_cfg *m_commonCfg;
	uint32_t		      m_notify_off_multiplier;
	uint8_t			     *isr;
	void			     *device_cfg;
};

typedef struct dk_virtio_queue {
	vm_page_t	   *page;
	struct vring_desc  *desc;
	struct vring_avail *avail;
	struct vring_used  *used;
} dk_virtio_queue_t;

@interface VirtIODevice : DKDevice {
	struct dk_virtio_info info;
}

+ (BOOL)probeWithPCIInfo:(dk_device_pci_info_t *)pciInfo;

- initWithVirtIOInfo:(struct dk_virtio_info *)info;
- (BOOL)exchangeFeatures:(uint64_t)requiredFeatures;
- (int)setupQueue:(dk_virtio_queue_t *)queue index:(uint16_t)index;
-(void)enableDevice;

@end

#endif /* VIRTIODEVICE_H_ */
