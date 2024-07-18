#include "ddk/virtio_pcireg.h"
#include "ddk/virtioreg.h"
#include "dev/PCIBus.h"
#include "dev/virtio/DKVirtIOPCITransport.h"
#include "dev/virtio/VirtIODisk.h"
#include "dev/virtio/VirtIO9pPort.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"

#if defined (__amd64__)
#include "kdk/amd64.h"
#include "dev/amd64/IOAPIC.h"
#endif

#if defined(__aarch64__) || defined(__amd64__)
@interface
DKVirtIOPCITransport (Private)
- (instancetype)initWithPCIBus:(PCIBus *)provider
			  info:(struct pci_dev_info *)info;
@end

#define PROVIDER ((PCIBus *)m_provider)

static int counter = 0;

@implementation DKVirtIOPCITransport

static void
dpc_handler(void *arg)
{
	DKVirtIOPCITransport *self = arg;
	[self->m_delegate deferredProcessing];
}

@synthesize delegate = m_delegate;

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info
{
	[[self alloc] initWithPCIBus:provider info:info];
	return YES;
}

- (void)capabilityEnumeratedAtOffset:(voff_t)capOffset
{
	struct virtio_pci_cap cap;

	cap.cap_vndr = PCIINFO_CFG_READ(b, &m_pciInfo, capOffset);

	if (cap.cap_vndr != 0x9)
		return;

	cap.cap_len = PCIINFO_CFG_READ(b, &m_pciInfo,
	    capOffset + offsetof(struct virtio_pci_cap, cap_len));
	cap.cfg_type = PCIINFO_CFG_READ(b, &m_pciInfo,
	    capOffset + offsetof(struct virtio_pci_cap, cfg_type));
	cap.bar = PCIINFO_CFG_READ(b, &m_pciInfo,
	    capOffset + offsetof(struct virtio_pci_cap, bar));
	cap.offset = PCIINFO_CFG_READ(l, &m_pciInfo,
	    capOffset + offsetof(struct virtio_pci_cap, offset));
	cap.length = PCIINFO_CFG_READ(l, &m_pciInfo,
	    capOffset + offsetof(struct virtio_pci_cap, length));

	switch (cap.cfg_type) {
	case VIRTIO_PCI_CAP_COMMON_CFG: {
		paddr_t phys = [PCIBus getBar:cap.bar forInfo:&m_pciInfo] +
		    cap.offset;
		m_commonCfg = (struct virtio_pci_common_cfg *)(P2V(phys));
		break;
	}

	case VIRTIO_PCI_CAP_NOTIFY_CFG: {
		paddr_t phys = [PCIBus getBar:cap.bar forInfo:&m_pciInfo] +
		    cap.offset;
		m_notifyBase = P2V(phys);
		m_notifyOffMultiplier = PCIINFO_CFG_READ(l, &m_pciInfo,
		    capOffset + sizeof(struct virtio_pci_cap));
		break;
	}

	case VIRTIO_PCI_CAP_ISR_CFG: {
		paddr_t phys = [PCIBus getBar:cap.bar forInfo:&m_pciInfo] +
		    cap.offset;
		m_isr = (uint8_t *)(P2V(phys));
		break;
	}

	case VIRTIO_PCI_CAP_DEVICE_CFG: {
		paddr_t phys = [PCIBus getBar:cap.bar forInfo:&m_pciInfo] +
		    cap.offset;
		m_deviceCfg = (void *)(P2V(phys));
		break;
	}

	case VIRTIO_PCI_CAP_PCI_CFG:
		/* epsilon */
		break;

	case VIRTIO_PCI_CAP_SHARED_MEMORY_CFG: {
		paddr_t phys = [PCIBus getBar:cap.bar forInfo:&m_pciInfo] +
		    cap.offset;
		DKLog("VirtIODevice", "Shared Memory is at 0x%zx\n", phys);
		break;
	}

	default:
		DKLog("VirtIODevice", "unknown capability config type %d\n",
		    cap.cfg_type);
	}
}

- (instancetype)initWithPCIBus:(PCIBus *)provider
			  info:(struct pci_dev_info *)info
{
	self = [super initWithProvider:provider];
	kmem_asprintf(obj_name_ptr(self), "virtio-pci-%u", counter++);
	[self registerDevice];

	m_pciInfo = *info;
	m_dpc.arg = self;
	m_dpc.callback = dpc_handler;
	m_dpc.cpu = NULL;

	[PCIBus enableBusMasteringForInfo:info];
	[PCIBus setMemorySpaceForInfo:info enabled:false];
	[PCIBus enumerateCapabilitiesForInfo:info delegate:self];
	[PCIBus setMemorySpaceForInfo:info enabled:true];

	DKLogAttach(self);

	switch (info->deviceId) {
	case 0x1001:
		[VirtIODisk probeWithProvider:self];
		break;

	case 0x1009:
		[VirtIO9pPort probeWithProvider:self];
		break;
	}

	return self;
}

- (volatile void *)deviceConfig
{
	return m_deviceCfg;
}

- (void)resetDevice
{
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_RESET;
	__sync_synchronize();
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_ACK;
	__sync_synchronize();
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER;
	__sync_synchronize();
}

- (BOOL)exchangeFeatures:(uint64_t)required
{
	for (int i = 0; i < 2; i++) {
		uint32_t requiredFeaturesPart = required >> (i * 32);

		m_commonCfg->device_feature_select = i;
		__sync_synchronize();
		if ((m_commonCfg->device_feature & requiredFeaturesPart) !=
		    requiredFeaturesPart) {
			DKDevLog(self,
			    "Unsupported features (dword %d): %x VS %x\n", i,
			    m_commonCfg->device_feature, requiredFeaturesPart);
			return false;
		}
		m_commonCfg->guest_feature_select = i;
		__sync_synchronize();
		m_commonCfg->guest_feature = requiredFeaturesPart;
		__sync_synchronize();
	}

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
	__sync_synchronize();
	if (m_commonCfg->device_status !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		DKDevLog(self, "Features OK not set.\n");
		return NO;
	}

	return YES;
}

- (int)setupQueue:(struct virtio_queue *)queue index:(uint16_t)index
{
	int r;
	vaddr_t addr;
	vaddr_t offs;

	r = vm_page_alloc(&queue->page, 0, kPageUseKWired, true);
	kassert(r == 0);
	addr = (vaddr_t)vm_page_direct_map_addr(queue->page);

	/* allocate a queue of total size 3336 bytes, to nicely fit in a page */
	queue->index = index;
	queue->length = 128;
	queue->last_seen_used = 0;

	ke_spinlock_init(&queue->spinlock);

	/* array of 128 vring_descs; amounts to 2048 bytes */
	queue->desc = (struct vring_desc *)addr;
	offs = sizeof(struct vring_desc) * 128;

	/* vring_avail with 128 rings; amounts to 260 bytes) */
	queue->avail = (struct vring_avail *)(addr + offs);
	offs += sizeof(struct vring_avail) +
	    sizeof(queue->avail->ring[0]) * 128;

	/* vring_used with 128 rings; amounts to ) */
	queue->used = (struct vring_used *)(addr + offs);

	memset((void *)addr, 0x0, 4096);

	for (int i = 0; i < queue->length; i++)
		queue->desc[i].next = i + 1;
	queue->free_desc_index = 0;
	queue->nfree_descs = 128;

	m_commonCfg->queue_select = index;
	__sync_synchronize();
	queue->notify_off = m_commonCfg->queue_notify_off;
	m_commonCfg->queue_desc = (uint64_t)V2P(queue->desc);
	m_commonCfg->queue_avail = (uint64_t)V2P(queue->avail);
	m_commonCfg->queue_used = (uint64_t)V2P(queue->used);
	m_commonCfg->queue_size = 128;
	__sync_synchronize();
	m_commonCfg->queue_enable = 1;

	__sync_synchronize();

	return 0;
}

- (void)enqueueDPC
{
	ke_dpc_enqueue(&self->m_dpc);
}

static bool
vitrio_handler(md_intr_frame_t *, void *arg)
{
	DKVirtIOPCITransport *self = arg;
	uint8_t isr_status = *self->m_isr;

	if ((isr_status & 3) == 0)
		/* not for us */
		return false;

	ke_dpc_enqueue(&self->m_dpc);

	return true;
}

- (int)enableDevice
{
	int r = 0;

#if defined(__amd64__)
	r = [IOApic handleGSI:m_pciInfo.gsi
		  withHandler:vitrio_handler
		     argument:self
		isLowPolarity:m_pciInfo.lopol
	      isEdgeTriggered:m_pciInfo.edge
		   atPriority:kIPLHigh
			entry:&m_intxEntry];
#else
	r = -1;
#endif
	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
		return r;
	}

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();

	[PCIBus setInterruptsEnabled:YES forInfo:&m_pciInfo];

	return 0;
}

- (int)allocateDescNumOnQueue:(struct virtio_queue *)queue
{
	int r;

	r = queue->free_desc_index;
	kassert(r != queue->length);
	queue->free_desc_index = QUEUE_DESC_AT(queue, r).next;
	queue->nfree_descs--;

	return r;
}

- (void)freeDescNum:(uint16_t)descNum onQueue:(struct virtio_queue *)queue
{
	QUEUE_DESC_AT(queue, descNum).next = queue->free_desc_index;
	queue->free_desc_index = descNum;
	queue->nfree_descs++;
}

- (void)submitDescNum:(uint16_t)descNum toQueue:(struct virtio_queue *)queue
{
#if DEBUG_VIRTIO > 2
	kprintf("Current index: %u\n Writing New Index: %u\n",
	    le16_to_native(queue->avail->idx) % queue->length,
	    native_to_le16(le16_to_native(queue->avail->idx) + 1));
#endif
	queue->avail->ring[le16_to_native(queue->avail->idx) % queue->length] =
	    native_to_le16(descNum);
	__sync_synchronize();
	queue->avail->idx = native_to_le16(
	    le16_to_native(queue->avail->idx) + 1);
	__sync_synchronize();
}

- (void)notifyQueue:(struct virtio_queue *)queue
{
	uint32_t *addr = (uint32_t *)(m_notifyBase +
	    queue->notify_off * m_notifyOffMultiplier);
	uint32_t value = queue->index;
	*addr = value;
	__sync_synchronize();
}

@end
#endif
