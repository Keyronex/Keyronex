#ifndef VIRTIODEVICE_H_
#define VIRTIODEVICE_H_

#include <dev/PCIBus.h>

#include <nanokern/thread.h>

struct vring_used_elem;

struct dk_virtio_info {
	dk_device_pci_info_t	      pciInfo;
	struct virtio_pci_common_cfg *m_commonCfg;
	uint8_t			     *isr;
	void			     *device_cfg;
	vaddr_t			      notify_base;

	uint32_t m_notify_off_multiplier;

	uint16_t		 num_queues;
	struct dk_virtio_queue **queues;
};

typedef struct dk_virtio_queue {
	/* queue number/index */
	uint16_t num;
	/* length of uqeue */
	uint16_t length;

	/* descriptor allocation semaphore */
	ksemaphore_t free_sem;
	/* manipulation spinlock */
	kspinlock_t spinlock;

	/* index of first free descriptor */
	uint16_t free_desc_index;
	/* last seen used index */
	uint16_t last_seen_used;

	/* page out of which desc, avail, used are allocated */
	vm_page_t *page;

	/* virtual address of descriptor array */
	struct vring_desc *desc;
	/* virtual address of driver ring */
	struct vring_avail *avail;
	/* virtual address of device ring */
	struct vring_used *used;
	/* notification offset */
	uint16_t notify_off;
} dk_virtio_queue_t;

#define QUEUE_DESC_AT(PQUEUE, IDX) ((PQUEUE)->desc[IDX])

@interface VirtIODevice : DKDevice {
	struct dk_virtio_info info;
}

+ (BOOL)probeWithPCIInfo:(dk_device_pci_info_t *)pciInfo;

/*!
 * Do common VirtIO initialiasation.
 */
- initWithVirtIOInfo:(struct dk_virtio_info *)info;
/*!
 *
 */
- (BOOL)exchangeFeatures:(uint64_t)requiredFeatures;
/*!
 * Initialise a queue and set it up with the device.
 */
- (int)setupQueue:(dk_virtio_queue_t *)queue index:(uint16_t)index;
/*!
 * Switch the device to normal operating mode.
 */
- (void)enableDevice;
/*!
 * Allocate a descriptor on a queue and return its index.
 * Waits forever until a descriptor is available - check you are not exceeding
 * the limit!
 * \pre queue spinlock NOT held
 */
- (uint16_t)allocateDescNumOnQueue:(dk_virtio_queue_t *)queue;
/*!
 * Free a descriptor of a queue by its index.
 * \pre queue spinlock HELD
 */
- (void)freeDescNum:(uint16_t)descNum onQueue:(dk_virtio_queue_t *)queue;
/*!
 * Submit a descriptor to a queue.
 * \pre queue spinlock HELD
 */
-(void)submitDescNum: (uint16_t)descNum toQueue:(dk_virtio_queue_t*) queue;
/*!
 * Notify a queue that new entries have been enqueued.
 */
- (void)notifyQueue:(dk_virtio_queue_t *)queue;
/*!
 * Handle an interrupt. Called at IPL=dispatch
 */
- (void)handleInterrupt;
/*!
 * Subclass responsibility.
 */
- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)queue;

@end

#endif /* VIRTIODEVICE_H_ */
