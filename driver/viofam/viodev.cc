/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#include "dev/virtioreg.h"
#include "kdk/libkern.h"

#include "viodev.hh"

typedef uint8_t u8;
typedef uint16_t le16;
typedef uint32_t le32;
typedef uint64_t le64;
#define le16_to_cpu(val) val

struct virtio_pci_cap {
	u8 cap_vndr;   /* Generic PCI field: PCI_CAP_ID_VNDR */
	u8 cap_next;   /* Generic PCI field: next ptr. */
	u8 cap_len;    /* Generic PCI field: capability length */
	u8 cfg_type;   /* Identifies the structure. */
	u8 bar;	       /* Where to find it. */
	u8 padding[3]; /* Pad to full dword. */
	le32 offset;   /* Offset within bar. */
	le32 length;   /* Length of the structure, in bytes. */
};

/* Common configuration */
#define VIRTIO_PCI_CAP_COMMON_CFG 1
/* Notifications */
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
/* ISR Status */
#define VIRTIO_PCI_CAP_ISR_CFG 3
/* Device specific configuration */
#define VIRTIO_PCI_CAP_DEVICE_CFG 4
/* PCI configuration access */
#define VIRTIO_PCI_CAP_PCI_CFG 5

struct virtio_pci_notify_cap {
	struct virtio_pci_cap cap;
	le32 notify_off_multiplier; /* Multiplier for queue_notify_off. */
};

struct virtio_pci_common_cfg {
	/* About the whole device. */
	le32 device_feature_select; /* read-write */
	le32 device_feature;	    /* read-only for driver */
	le32 driver_feature_select; /* read-write */
	le32 driver_feature;	    /* read-write */
	le16 msix_config;	    /* read-write */
	le16 num_queues;	    /* read-only for driver */
	u8 device_status;	    /* read-write */
	u8 config_generation;	    /* read-only for driver */

	/* About a specific virtqueue. */
	le16 queue_select;	/* read-write */
	le16 queue_size;	/* read-write */
	le16 queue_msix_vector; /* read-write */
	le16 queue_enable;	/* read-write */
	le16 queue_notify_off;	/* read-only for driver */
	le64 queue_desc;	/* read-write */
	le64 queue_driver;	/* read-write */
	le64 queue_device;	/* read-write */
};

#if 0
static bool
virtio_intr(md_intr_frame_t *frame, void *arg)
{
	VirtIODevice *dev = arg;
	return [dev handleInterrupt];
}
#endif

void
VirtIODevice::enumerateCapabilitiesCallback(pci_device_info *info, voff_t pCap,
    void *arg)
{
	struct virtio_pci_cap cap;
	VirtIODevice *dev = (VirtIODevice *)arg;

	cap.cap_vndr = PCIINFO_CFG_READ(b, &dev->pci_info, pCap);

	if (cap.cap_vndr != 0x9)
		return;

	cap.cap_len = PCIINFO_CFG_READ(b, &dev->pci_info,
	    pCap + offsetof(struct virtio_pci_cap, cap_len));
	cap.cfg_type = PCIINFO_CFG_READ(b, &dev->pci_info,
	    pCap + offsetof(struct virtio_pci_cap, cfg_type));
	cap.bar = PCIINFO_CFG_READ(b, &dev->pci_info,
	    pCap + offsetof(struct virtio_pci_cap, bar));
	cap.offset = PCIINFO_CFG_READ(d, &dev->pci_info,
	    pCap + offsetof(struct virtio_pci_cap, offset));
	cap.length = PCIINFO_CFG_READ(d, &dev->pci_info,
	    pCap + offsetof(struct virtio_pci_cap, length));

	switch (cap.cfg_type) {
	case VIRTIO_PCI_CAP_COMMON_CFG:
		dev->m_common_cfg = (virtio_pci_common_cfg
			*)((char *)P2V(
			       PCIDevice::getBar(dev->pci_info, cap.bar)) +
		    cap.offset);
		break;

	case VIRTIO_PCI_CAP_NOTIFY_CFG:
		dev->notify_base = (vaddr_t)((char *)P2V(PCIDevice::getBar(
						 dev->pci_info, cap.bar)) +
		    cap.offset);
		dev->m_notify_off_multiplier = PCIINFO_CFG_READ(d,
		    &dev->pci_info, pCap + sizeof(struct virtio_pci_cap));
		break;

	case VIRTIO_PCI_CAP_ISR_CFG:
		dev->isr = (uint8_t *)((char *)P2V(
					   PCIDevice::getBar(dev->pci_info,
					       cap.bar)) +
		    cap.offset);
		break;

	case VIRTIO_PCI_CAP_DEVICE_CFG:
		dev->device_cfg = ((char *)P2V(PCIDevice::getBar(dev->pci_info,
				       cap.bar)) +
		    cap.offset);
		break;

	case VIRTIO_PCI_CAP_PCI_CFG:
		/* epsilon */
		break;

	default:
		DKLog("VirtIODevice", "unknown capability config type %d\n",
		    cap.cfg_type);
	}
}

VirtIODevice::VirtIODevice(PCIDevice *provider, pci_device_info &info)
    : pci_info(info)
{
#if 0
	[PCIBus enableMemorySpace:pciInfo];
	[PCIBus enableBusMastering:pciInfo];
#endif
	PCIDevice::enumerateCapabilities(info, enumerateCapabilitiesCallback,
	    this);

	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_RESET;
	__sync_synchronize();
	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_ACK;
	__sync_synchronize();
	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER;
	__sync_synchronize();
}

bool
VirtIODevice::exchangeFeatures(uint64_t required_mask)
{
	for (int i = 0; i < 2; i++) {
		uint32_t requiredFeaturesPart = required_mask >> (i * 32);
		m_common_cfg->device_feature_select = i;
		__sync_synchronize();
		if ((m_common_cfg->device_feature & requiredFeaturesPart) !=
		    requiredFeaturesPart) {
			DKDevLog(self,
			    "Unsupported features (dword %d): %x VS %x\n", i,
			    m_common_cfg->device_feature, requiredFeaturesPart);
			return false;
		}
		m_common_cfg->driver_feature_select = i;
		__sync_synchronize();
		m_common_cfg->driver_feature = requiredFeaturesPart;
		__sync_synchronize();
	}

	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
	__sync_synchronize();
	if (m_common_cfg->device_status !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		DKDevLog(self, "Features OK not set.\n");
		return false;
	}
	return true;
}

int
VirtIODevice::setupQueue(virtio_queue *queue, uint16_t index)
{
	vaddr_t addr;
	vaddr_t offs;

	// queue->page = vm_pagealloc(true, &vm_pgdevbufq);
	// addr =(vaddr_t)P2V(queue->page->paddr);

	/* allocate a queue of total size 3336 bytes, to nicely fit in a page */
	queue->num = index;
	queue->length = 128;

	ke_spinlock_init(&queue->spinlock);
	ke_semaphore_init(&queue->free_sem, queue->length);

	/* array of 128 vring_descs; amounts to 2048 bytes */
	queue->desc = (vring_desc *)addr;
	offs = sizeof(struct vring_desc) * 128;

	/* vring_avail with 128 rings; amounts to 260 bytes) */
	queue->avail = (vring_avail *)(addr + offs);
	offs += sizeof(struct vring_avail) +
	    sizeof(queue->avail->ring[0]) * 128;

	/* vring_used with 128 rings; amounts to ) */
	queue->used = (vring_used *)(addr + offs);

	memset((void *)addr, 0x0, 4096);

	for (int i = 0; i < queue->length; i++)
		queue->desc[i].next = i + 1;
	queue->free_desc_index = 0;

	m_common_cfg->queue_select = index;
	__sync_synchronize();
	queue->notify_off = m_common_cfg->queue_notify_off;
	m_common_cfg->queue_desc = (uint64_t)V2P(queue->desc);
	m_common_cfg->queue_driver = (uint64_t)V2P(queue->avail);
	m_common_cfg->queue_device = (uint64_t)V2P(queue->used);
	m_common_cfg->queue_size = 128;
	__sync_synchronize();
	m_common_cfg->queue_enable = 1;

	__sync_synchronize();

	return 0;
}

int
VirtIODevice::enableDevice()
{
	int r;

#if 0
	r = [PCIBus handleInterruptOf:&info.pciInfo
			  withHandler:virtio_intr
			     argument:self
			   atPriority:kSPLBIO];
#endif

	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
		return r;
	}

	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();
}

#if 0

#endif

#if 0

- (uint16_t)allocateDescNumOnQueue:(dk_virtio_queue_t *)queue
{
	int r;
	ipl_t ipl;

	r = nk_wait(&queue->free_sem, "virtio_queue_free", false, false, -1);
	kassert(r == kKernWaitStatusOK);

	ipl = nk_spinlock_acquire_at(&queue->spinlock, kSPLBIO);
	r = queue->free_desc_index;
	kassert(r != queue->length);
	queue->free_desc_index = QUEUE_DESC_AT(queue, r).next;
	nk_spinlock_release(&queue->spinlock, ipl);

	return r;
}

- (void)freeDescNum:(uint16_t)descNum onQueue:(dk_virtio_queue_t *)queue
{
	QUEUE_DESC_AT(queue, descNum).next = queue->free_desc_index;
	queue->free_desc_index = descNum;

	nk_semaphore_release(&queue->free_sem, 1);
}

- (void)submitDescNum:(uint16_t)descNum toQueue:(dk_virtio_queue_t *)queue
{
	queue->avail->ring[queue->avail->idx % queue->length] = descNum;
	__sync_synchronize();
	queue->avail->idx += 1;
	__sync_synchronize();
}

- (void)notifyQueue:(dk_virtio_queue_t *)queue
{
#if 0 /* only needed with VIRTIO_F_NOTIFICATION_DATA */
	/*
	le32 {
	vqn : 16;
	next_off : 15;
	next_wrap : 1;
	};
	*/
	uint32_t value = queue->num << 16 | 0 << 1 | 0;
#endif

	uint32_t *addr = (uint32_t *)(info.notify_base +
	    queue->notify_off * info.m_notify_off_multiplier);
	uint32_t value = queue->num;
	*addr = value;
	__sync_synchronize();
}

- (bool)handleInterrupt
{
	uint8_t isr_status = *info.isr;

	if ((isr_status & 3) == 0)
		/* not for us */
		return false;

#if 0
	DKDevLog(self, "interrupted, processing VirtIO queues...\n");
#endif

	for (int i = 0; i < info.num_queues; i++) {
		dk_virtio_queue_t *queue = info.queues[i];
		uint16_t i;
		ipl_t ipl;

		ipl = nk_spinlock_acquire_at(&queue->spinlock, kSPLBIO);

		for (i = queue->last_seen_used;
		     i != queue->used->idx % queue->length;
		     i = (i + 1) % queue->length) {
			struct vring_used_elem *e =
			    &queue->used->ring[i % queue->length];

			/*
			 * ugly hack, maybe a problem: but need to access sched
			 * from our handlers for now. i think we can get
			 * away without releasing the spinlock as we won't get
			 * another interrupt for this device on this cpu.
			 *
			 * todo(med): refactor the processBuffer:onQueue:
			 * implementations to use DPCs to do any scheduler
			 * stuff.
			 */
			// nk_spinlock_release(&queue->spinlock, ipl);
			splx(kSPLDispatch);
			[self processBuffer:e onQueue:queue];
			splbio();
			// ipl = nk_spinlock_acquire_at(&queue->spinlock,
			// kSPLBIO);
		}

		nk_spinlock_release(&queue->spinlock, ipl);

		queue->last_seen_used = i;
	}

	return true;
}

- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)queue
{
	kfatal("subclass responsibility\n");
}

@end
#endif