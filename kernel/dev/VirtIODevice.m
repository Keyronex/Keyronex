#include "VirtIODevice.h"
#include "md/intr.h"
#include "virtioreg.h"

typedef uint8_t	 u8;
typedef uint16_t le16;
typedef uint32_t le32;
typedef uint64_t le64;

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

static void virtio_intr(md_intr_frame_t *frame, void *arg) {
	kprintf("GOT A VIRTIO INT!\n");
	for (;;) ;
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

	DKLog("VirtIODevice",
	    "Capability: cfg_type %d, bar %d, offset %d, length %d\n",
	    cap.cfg_type, cap.bar, cap.offset, cap.length);

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

	info.m_commonCfg->queue_select = index;
	__sync_synchronize();
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
			   atPriority:kSPL0];

	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
	}
}

- (void)notifyQueue:(dk_virtio_queue_t *)queue
{
	/*
	le32 {
	vqn : 16;
	next_off : 15;
	next_wrap : 1;
	};
	*/
	uint32_t value = queue->num << 16 | 0 << 1 | 0;
	uint32_t *addr = (uint32_t*)(info.notify_base + 0 * info.m_notify_off_multiplier);
	*addr = value;
	__sync_synchronize();

	for (;;) ;
}

@end
