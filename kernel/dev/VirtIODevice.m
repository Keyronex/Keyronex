#include "VirtIODevice.h"
#include "libkern/libkern.h"
#include "md/intr.h"
#include "md/spl.h"
#include "nanokern/thread.h"
#include "virtioreg.h"

typedef uint8_t	 u8;
typedef uint16_t le16;
typedef uint32_t le32;
typedef uint64_t le64;
#define le16_to_cpu(val) val

struct virtio_pci_cap {
	u8   cap_vndr;	 /* Generic PCI field: PCI_CAP_ID_VNDR */
	u8   cap_next;	 /* Generic PCI field: next ptr. */
	u8   cap_len;	 /* Generic PCI field: capability length */
	u8   cfg_type;	 /* Identifies the structure. */
	u8   bar;	 /* Where to find it. */
	u8   padding[3]; /* Pad to full dword. */
	le32 offset;	 /* Offset within bar. */
	le32 length;	 /* Length of the structure, in bytes. */
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
	u8   device_status;	    /* read-write */
	u8   config_generation;	    /* read-only for driver */

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

#undef malloc
void *
malloc(size_t size)
{
	return liballoc_kmalloc(size);
}

static void
virtio_intr(md_intr_frame_t *frame, void *arg)
{
	VirtIODevice *dev = arg;
	[dev handleInterrupt];
}

@implementation VirtIODevice

void
enumerateCaps(dk_device_pci_info_t *pciInfo, voff_t pCap, void *arg)
{
	struct virtio_pci_cap  cap;
	struct dk_virtio_info *dev = arg;

	cap.cap_vndr = PCIINFO_CFG_READ(b, pciInfo, pCap);

	if (cap.cap_vndr != 0x9)
		return;

	cap.cap_len = PCIINFO_CFG_READ(b, pciInfo,
	    pCap + offsetof(struct virtio_pci_cap, cap_len));
	cap.cfg_type = PCIINFO_CFG_READ(b, pciInfo,
	    pCap + offsetof(struct virtio_pci_cap, cfg_type));
	cap.bar = PCIINFO_CFG_READ(b, pciInfo,
	    pCap + offsetof(struct virtio_pci_cap, bar));
	cap.offset = PCIINFO_CFG_READ(d, pciInfo,
	    pCap + offsetof(struct virtio_pci_cap, offset));
	cap.length = PCIINFO_CFG_READ(d, pciInfo,
	    pCap + offsetof(struct virtio_pci_cap, length));

	switch (cap.cfg_type) {
	case VIRTIO_PCI_CAP_COMMON_CFG:
		dev->m_commonCfg = (P2V([PCIBus getBar:cap.bar info:pciInfo]) +
		    cap.offset);
		break;

	case VIRTIO_PCI_CAP_NOTIFY_CFG:
		dev->notify_base = (vaddr_t)(P2V([PCIBus getBar:cap.bar
							   info:pciInfo]) +
		    cap.offset);
		dev->m_notify_off_multiplier = PCIINFO_CFG_READ(d, pciInfo,
		    pCap + sizeof(struct virtio_pci_cap));
		break;

	case VIRTIO_PCI_CAP_ISR_CFG:
		dev->isr = (P2V([PCIBus getBar:cap.bar info:pciInfo]) +
		    cap.offset);
		break;

	case VIRTIO_PCI_CAP_DEVICE_CFG:
		dev->device_cfg = (P2V([PCIBus getBar:cap.bar info:pciInfo]) +
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

+ (BOOL)probeWithPCIInfo:(dk_device_pci_info_t *)pciInfo
{
	struct dk_virtio_info vioInfo;

	[PCIBus enableMemorySpace:pciInfo];
	[PCIBus enableBusMastering:pciInfo];

	[PCIBus enumerateCapabilitiesOf:pciInfo
			   withCallback:enumerateCaps
			       userData:&vioInfo];

	vioInfo.pciInfo = *pciInfo;

	[[self alloc] initWithVirtIOInfo:&vioInfo];

	return YES;
}

- initWithVirtIOInfo:(struct dk_virtio_info *)vioInfo;
{
	self = [super initWithProvider:vioInfo->pciInfo.busObj];

	info = *vioInfo;

	vioInfo->m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_RESET;
	__sync_synchronize();
	vioInfo->m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_ACK;
	__sync_synchronize();
	vioInfo->m_commonCfg->device_status =
	    VIRTIO_CONFIG_DEVICE_STATUS_DRIVER;
	__sync_synchronize();

	return self;
}

- (BOOL)exchangeFeatures:(uint64_t)requiredFeatures
{

	for (int i = 0; i < 2; i++) {
		uint32_t requiredFeaturesPart = requiredFeatures >> (i * 32);
		info.m_commonCfg->device_feature_select = i;
		__sync_synchronize();
		if ((info.m_commonCfg->device_feature & requiredFeaturesPart) !=
		    requiredFeaturesPart) {
			DKDevLog(self,
			    "Unsupported features (dword %d): %x VS %x\n", i,
			    info.m_commonCfg->device_feature,
			    requiredFeaturesPart);
			return NO;
		}
		info.m_commonCfg->driver_feature_select = i;
		__sync_synchronize();
		info.m_commonCfg->driver_feature = requiredFeaturesPart;
		__sync_synchronize();
	}

	info.m_commonCfg->device_status =
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
	__sync_synchronize();
	if (info.m_commonCfg->device_status !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		DKDevLog(self, "Features OK not set.\n");
		return NO;
	}
	return YES;
}

- (int)setupQueue:(dk_virtio_queue_t *)queue index:(uint16_t)index
{
	queue->page = vm_pagealloc(true, &vm_pgkmemq);
	vaddr_t addr = (vaddr_t)P2V(queue->page->paddr);
	vaddr_t offs;

	/* allocate a queue of total size 3336 bytes, to nicely fit in a page */
	queue->num = index;
	queue->length = 128;

	nk_spinlock_init(&queue->spinlock);
	nk_semaphore_init(&queue->free_sem, queue->length);

	/* array of 128 vring_descs; amounts to 2048 bytes */
	queue->desc = (void *)addr;
	offs = sizeof(struct vring_desc) * 128;

	/* vring_avail with 128 rings; amounts to 260 bytes) */
	queue->avail = (void *)(addr + offs);
	offs += sizeof(struct vring_avail) +
	    sizeof(queue->avail->ring[0]) * 128;

	/* vring_used with 128 rings; amounts to ) */
	queue->used = (void *)(addr + offs);

	memset((void *)addr, 0x0, 4096);

	for (int i = 0; i < queue->length; i++)
		queue->desc[i].next = i + 1;
	queue->free_desc_index = 0;

	info.m_commonCfg->queue_select = index;
	__sync_synchronize();
	queue->notify_off = info.m_commonCfg->queue_notify_off;
	info.m_commonCfg->queue_desc = (uint64_t)V2P(queue->desc);
	info.m_commonCfg->queue_driver = (uint64_t)V2P(queue->avail);
	info.m_commonCfg->queue_device = (uint64_t)V2P(queue->used);
	info.m_commonCfg->queue_size = 128;
	__sync_synchronize();
	info.m_commonCfg->queue_enable = 1;

	__sync_synchronize();

	return 0;
}

- (void)enableDevice
{
	info.m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();

	int r = [PCIBus handleInterruptOf:&info.pciInfo
			      withHandler:virtio_intr
				 argument:self
			       atPriority:kSPLBIO];

	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
	}
}

- (uint16_t)allocateDescNumOnQueue:(dk_virtio_queue_t *)queue
{
	int   r;
	ipl_t ipl;

	r = nk_wait(&queue->free_sem, "virtio_queue_free", false, false, -1);
	kassert(r == kKernWaitStatusOK);

	ipl = nk_spinlock_acquire(&queue->spinlock);
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
	    0 * info.m_notify_off_multiplier);
	uint32_t  value = queue->num;
	*addr = value;
	__sync_synchronize();
}

- (void)handleInterrupt
{
	uint8_t isr_status = *info.isr;

	DKDevLog(self, "interrupted, processing VirtIO queues...\n");

	for (int i = 0; i < info.num_queues; i++) {
		dk_virtio_queue_t *queue = info.queues[i];
		uint16_t	   i;
		ipl_t		   ipl;

		ipl = nk_spinlock_acquire_at(&queue->spinlock, kSPLBIO);

		for (i = queue->last_seen_used;
		     i != queue->used->idx % queue->length;
		     i = (i + 1) % queue->length) {
			struct vring_used_elem *e =
			    &queue->used
				 ->ring[queue->last_seen_used % queue->length];

			/*
			 * ugly hack, maybe a problem: but need to access sched
			 * from our handlers, unfortunately. i think we can get
			 * away without releasing the spinlock as we won't get
			 * another interrupt for this device on this cpu.
			 * todo(med): refactor to use DPCs to do the scheduler
			 * stuff (or require the completion callback to be happy
			 * running at arbitrary IPL?)
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
}

- (void)processBuffer:(struct vring_used_elem *)e
	      onQueue:(dk_virtio_queue_t *)queue
{
	kfatal("subclass responsibility\n");
}

@end
