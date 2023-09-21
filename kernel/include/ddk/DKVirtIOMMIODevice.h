#ifndef KRX_DDK_DKVIRTIOMMIODEVICE_H
#define KRX_DDK_DKVIRTIOMMIODEVICE_H

#include "ddk/DKDevice.h"
#include "kdk/endian.h"
#include "kdk/vm.h"

struct vring_used_elem;
@class DKVirtIOMMIODevice;

struct virtio_queue {
	/* queue number/index */
	uint16_t index;
	/* length of uqeue */
	uint16_t length;

	/* manipulation spinlock */
	kspinlock_t spinlock;

	/* index of first free descriptor */
	uint16_t free_desc_index;
	/* number of free descriptors */
	uint16_t nfree_descs;
	/* last seen used index */
	uint16_t last_seen_used;

	/* page out of which desc, avail, used are allocated */
	vm_page_t *page;

	/* virtual address of descriptor array */
	volatile struct vring_desc *desc;
	/* virtual address of driver ring */
	volatile struct vring_avail *avail;
	/* virtual address of device ring */
	volatile struct vring_used *used;

	/* for PCI - notification offset */
	uint16_t notify_off;
};

#define QUEUE_DESC_AT(PQUEUE, IDX) ((PQUEUE)->desc[IDX])

@protocol DKVirtIODelegate
+ (BOOL)probeWithProvider:(DKVirtIOMMIODevice*) provider;

- (void)deferredProcessing;
@end

/*!
 * VirtIO MMIO device nub.
 */
@interface DKVirtIOMMIODevice : DKDevice {
    @public
	volatile void *m_mmio;
	int m_interrupt;
	kdpc_t m_dpc;
	id<DKVirtIODelegate> m_delegate;
}

@property (readonly) volatile void *deviceConfig;

+ (BOOL)probeWithProvider:(DKDevice *)provider
		     mmio:(volatile void *)mmio
		interrupt:(int)interrupt;

- (void)enqueueDPC;
- (void)resetDevice;
- (BOOL)exchangeFeatures:(uint64_t)required;
- (int)setupQueue:(struct virtio_queue *)queue index:(uint16_t)index;
- (int)enableDevice;
- (int)allocateDescNumOnQueue:(struct virtio_queue *)queue;
- (void)freeDescNum:(uint16_t)num onQueue:(struct virtio_queue *)queue;
- (void)submitDescNum:(uint16_t)descNum toQueue:(struct virtio_queue *)queue;
- (void)notifyQueue:(struct virtio_queue *)queue;

@end

#endif /* KRX_DDK_DKVIRTIOMMIODEVICE_H */
