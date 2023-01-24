#ifndef VIRTIODEVICE_H_
#define VIRTIODEVICE_H_

#include <dev/PCIBus.h>

struct dk_virtio_info {
	dk_device_pci_info_t	      pciInfo;
	struct virtio_pci_common_cfg *m_commonCfg;
	uint32_t		      m_notify_off_multiplier;
	uint8_t			     *isr;
	void			     *device_cfg;
	vaddr_t notify_base;
};

typedef struct dk_virtio_queue {
	uint16_t num;
	/* page out of which desc, avail, used are allocated */
	vm_page_t	   *page;
	/* virtual address of descriptor array */
	struct vring_desc  *desc;
	/* virtual address of driver ring */
	struct vring_avail *avail;
	/* virtual address of device ring */
	struct vring_used  *used;
	uint32_t notify_off;
} dk_virtio_queue_t;

@interface VirtIODevice : DKDevice {
	struct dk_virtio_info info;
}

+ (BOOL)probeWithPCIInfo:(dk_device_pci_info_t *)pciInfo;

- initWithVirtIOInfo:(struct dk_virtio_info *)info;
- (BOOL)exchangeFeatures:(uint64_t)requiredFeatures;
- (int)setupQueue:(dk_virtio_queue_t *)queue index:(uint16_t)index;
- (void)enableDevice;
-(void)notifyQueue:(dk_virtio_queue_t *)queue;

@end

#endif /* VIRTIODEVICE_H_ */
