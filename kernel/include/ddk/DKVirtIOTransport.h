/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Sat May 4 2024.
 */


#ifndef KRX_DDK_DKVIRTIOTRANSPORT_H
#define KRX_DDK_DKVIRTIOTRANSPORT_H

#include <kdk/kern.h>
#include <kdk/vm.h>

#include <ddk/DKDevice.h>

struct vring_used_elem;
@class DKVirtIOTransport;

#define QUEUE_DESC_AT(PQUEUE, IDX) ((PQUEUE)->desc[IDX])

typedef struct virtio_queue {
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

	/* pci */
	uint32_t notify_off;
	uint16_t pci_msix_vec;
} virtio_queue_t;

@protocol DKVirtIODevice
- (instancetype)initWithTransport:(DKVirtIOTransport *)transport;
- (void)additionalDeferredProcessingForQueue:(virtio_queue_t *)queue;
- (void)processUsedDescriptor:(volatile struct vring_used_elem *)e
		      onQueue:(struct virtio_queue *)queue;
@end

@interface DKVirtIOTransport: DKDevice

@property (readonly) volatile void *deviceConfig;

- (void)resetDevice;
- (int)enableDevice;
- (bool)exchangeFeaturesMandatory:(uint64_t)mandatory
			 optional:(uint64_t *)optional;
- (int)setupQueue:(virtio_queue_t *)queue index:(uint16_t)index;
- (int)allocateDescNumOnQueue:(struct virtio_queue *)queue;
- (void)freeDescNum:(uint16_t)num onQueue:(struct virtio_queue *)queue;
- (void)submitDescNum:(uint16_t)descNum toQueue:(struct virtio_queue *)queue;
- (void)notifyQueue:(struct virtio_queue *)queue;

@end

#endif /* KRX_DDK_DKVIRTIOTRANSPORT_H */
