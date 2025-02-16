/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Sat May 4 2024.
 */
/*
 * @file VirtIOPCITransport.m
 * @brief VirtIO transport for PCI.
 */

#include <ddk/DKPCIDevice.h>
#include <ddk/DKVirtIOTransport.h>
#include <ddk/reg/pci.h>
#include <ddk/safe_endian.h>
#include <ddk/virtio_pcireg.h>
#include <ddk/virtioreg.h>
#include <kdk/kern.h>
#include <kdk/libkern.h>
#include <kdk/vm.h>

#include "vm/vmp.h"

static bool intx_handler(md_intr_frame_t *, void *);
static void dpc_handler(void *);

@interface VirtIOPCITransport : DKVirtIOTransport <DKPCIDeviceMatching> {
    @public
	DKPCIDevice *m_pciDevice;
	DKDevice<DKVirtIODevice> *m_delegate;
	volatile struct virtio_pci_common_cfg *m_commonCfg;
	volatile uint8_t *m_isr;
	volatile void *m_devCfg;
	vaddr_t m_notify;
	size_t m_notifyOffMultiplier;

	struct intr_entry m_intxEntry;
	kdpc_t m_dpc;

	virtio_queue_t *m_queues;
	size_t m_queues_size;
}

@end

@implementation VirtIOPCITransport

@synthesize deviceConfig = m_devCfg;

+ (void)load
{
	[DKPCIDevice registerMatchingClass:self];
}

+ (uint8_t)probeWithMatchData:(struct DKPCIMatchData *)matchData
{
	if (matchData->vendor == 0x1af4 &&
	    (matchData->device >= 0x1000 && matchData->device <= 0x10ef))
		return 100;
	else
		return 0;
}

- (void)resetDevice
{
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_RESET;
	__sync_synchronize();
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_ACK;
	__sync_synchronize();
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER;
	__sync_synchronize();
	[m_pciDevice setMSIx:true];
}

- (int)enableDevice
{
	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();
#if 0

	r = [[platformDevice platformInterruptController]
	    handleSource:&m_pciInfo.intx_source
	     withHandler:vitrio_handler
		argument:self
	      atPriority:kIPLHigh
		   entry:&m_intxEntry];
	if (r < 0) {
		DKDevLog(self, "Failed to allocate interrupt handler: %d\n", r);
		return r;
	}

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();

	[DKPCIBus setInterruptsEnabled:YES forInfo:&m_pciInfo];
#endif
	return 0;
}

- (bool)exchangeFeaturesMandatory:(uint64_t)mandatory
			 optional:(uint64_t *)optional
{
	uint64_t negotiatedOptional = 0;

	for (int i = 0; i < 2; i++) {
		uint32_t mandatoryFeaturesPart = mandatory >> (i * 32);
		uint32_t optionalFeaturesPart = optional == NULL ?
		    0 :
		    (*optional >> (i * 32));
		uint32_t requiredFeaturesPart = mandatoryFeaturesPart |
		    optionalFeaturesPart;
		uint32_t negotiatedFeaturesPart;
		uint32_t deviceFeatures;

		m_commonCfg->device_feature_select = to_leu32(i);
		__sync_synchronize();

		deviceFeatures = from_leu32(m_commonCfg->device_feature);

		if ((deviceFeatures & mandatoryFeaturesPart) !=
		    mandatoryFeaturesPart) {
			kprintf("Unsupported mandatory features (dword %d): "
				"%x VS %x\n",
			    i, deviceFeatures, mandatoryFeaturesPart);
			return false;
		}

		negotiatedFeaturesPart = deviceFeatures & requiredFeaturesPart;
		negotiatedOptional |= ((uint64_t)negotiatedFeaturesPart
		    << (i * 32));

		m_commonCfg->guest_feature_select = to_leu32(i);
		__sync_synchronize();

		m_commonCfg->guest_feature = to_leu32(negotiatedFeaturesPart);
		__sync_synchronize();
	}

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
	__sync_synchronize();
	if (m_commonCfg->device_status !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		kprintf("Features OK not set.\n");
		return false;
	}

	if (optional != NULL)
		*optional = negotiatedOptional;

	return true;
}

- (void)setupQueue:(virtio_queue_t *)queue index:(uint16_t)index
{
	vaddr_t addr;
	vaddr_t offs;

	vm_page_alloc(&queue->page, 0, kPageUseKWired, true);
	addr = vm_page_direct_map_addr(queue->page);

	/* allocate a queue of total size 3336 bytes, to nicely fit in a page */
	queue->index = index;
	queue->length = 128;
	queue->last_seen_used = 0;

	ke_spinlock_init(&queue->spinlock);

	/* array of 128 vring_descs; amounts to 2048 bytes */
	queue->desc = (struct vring_desc *)addr;
	offs = sizeof(struct vring_desc) * 128;

	/* vring_avail with 128 rings; amounts to 260 bytes */
	queue->avail = (struct vring_avail *)(addr + offs);
	offs += sizeof(struct vring_avail) +
	    sizeof(queue->avail->ring[0]) * 128;

	/* vring_used with 128 rings; amounts to 1028 bytes) */
	queue->used = (struct vring_used *)(addr + offs);

	memset((void *)addr, 0x0, 4096);

	for (int i = 0; i < queue->length; i++)
		queue->desc[i].next = to_leu16(i + 1);
	queue->free_desc_index = 0;
	queue->nfree_descs = 128;

	m_commonCfg->queue_select = to_leu16(index);
	__sync_synchronize();

	queue->notify_off = from_leu16(m_commonCfg->queue_notify_off);
	m_commonCfg->queue_msix_vector = to_leu16(queue->pci_msix_vec);
	m_commonCfg->queue_desc = to_leu64(V2P((vaddr_t)queue->desc));
	m_commonCfg->queue_avail = to_leu64(V2P((vaddr_t)queue->avail));
	m_commonCfg->queue_used = to_leu64(V2P((vaddr_t)queue->used));
	m_commonCfg->queue_size = to_leu16(128);
	__sync_synchronize();

	m_commonCfg->queue_enable = to_leu16(1);

	__sync_synchronize();
}

- (int)allocateDescNumOnQueue:(struct virtio_queue *)queue
{
	int r;

	r = queue->free_desc_index;
	kassert(r != queue->length);
	queue->free_desc_index = from_leu16(QUEUE_DESC_AT(queue, r).next);
	queue->nfree_descs--;

	return r;
}

- (void)freeDescNum:(uint16_t)descNum onQueue:(struct virtio_queue *)queue
{
	QUEUE_DESC_AT(queue, descNum).next = to_leu16(queue->free_desc_index);
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
	queue->avail->ring[from_leu16(queue->avail->idx) % queue->length] =
	    to_leu16(descNum);
	__sync_synchronize();
	queue->avail->idx = to_leu16(from_leu16(queue->avail->idx) + 1);
	__sync_synchronize();
}

- (void)notifyQueue:(struct virtio_queue *)queue
{
	uint32_t *addr = (uint32_t *)(m_notify +
	    queue->notify_off * m_notifyOffMultiplier);
	uint32_t value = queue->index;
	*addr = value;
}

- (void)deferredProcessing
{
	/* process the virtqueues... */
}

- (void)mapCapabilities
{
	struct virtio_pci_cap cap;
	uint16_t capOffset = 0;
	DKPCIBarInfo bar;
	int r;

	[m_pciDevice setMemorySpace:false];
	while (true) {
		paddr_t paddr;
		vaddr_t vaddr;

		capOffset = [m_pciDevice findCapabilityByID:0x9
					     startingOffset:capOffset];
		if (capOffset == 0)
			break;

		cap.cap_len = [m_pciDevice
		    configRead8:capOffset +
		    offsetof(struct virtio_pci_cap, cap_len)];
		cap.cfg_type = [m_pciDevice
		    configRead8:capOffset +
		    offsetof(struct virtio_pci_cap, cfg_type)];
		cap.bar = [m_pciDevice configRead8:capOffset +
				       offsetof(struct virtio_pci_cap, bar)];
		cap.offset.value = [m_pciDevice
		    configRead32:capOffset +
		    offsetof(struct virtio_pci_cap, offset)];
		cap.length.value = [m_pciDevice
		    configRead32:capOffset +
		    offsetof(struct virtio_pci_cap, length)];

		if (cap.cfg_type == VIRTIO_PCI_CAP_PCI_CFG)
			continue;

		bar = [m_pciDevice barInfo:cap.bar];
		if (bar.type != kPCIBarMem) {
			kprintf("VirtIO PCI capability: bar %d is not memory\n",
			    cap.bar);
			continue;
		}

		paddr = bar.base + from_leu32(cap.offset);
		kassert(paddr % PGSIZE == 0);

		/* xxx: bad cache mode! */
		r = vm_ps_map_physical_view(&kernel_procstate, &vaddr,
		    ROUNDUP(from_leu32(cap.length), PGSIZE), paddr,
		    kVMRead | kVMWrite, kVMRead | kVMWrite, false);
		kassert(r == 0);

		switch (cap.cfg_type) {
		case VIRTIO_PCI_CAP_COMMON_CFG:
			m_commonCfg = (volatile void *)vaddr;
			break;

		case VIRTIO_PCI_CAP_NOTIFY_CFG:
			m_notify = vaddr;
			m_notifyOffMultiplier = [m_pciDevice
			    configRead32:capOffset +
			    sizeof(struct virtio_pci_cap)];
			break;

		case VIRTIO_PCI_CAP_DEVICE_CFG:
			m_devCfg = (volatile void *)vaddr;
			break;

		case VIRTIO_PCI_CAP_ISR_CFG:
			m_isr = (volatile uint8_t *)vaddr;
			break;
		}
	}
	[m_pciDevice setMemorySpace:true];

	if (m_commonCfg == NULL || m_notify == 0 || m_devCfg == NULL)
		kfatal("Required VirtIO PCI capabilities not found\n");
}

- (void)start
{
	[super start];
	[self mapCapabilities];

	[m_pciDevice setBusMastering:true];

	switch ([m_pciDevice configRead16:kDeviceID]) {
	case 0x1001:
	case 0x1040 + VIRTIO_DEVICE_ID_BLOCK:
		kprintf("Block\n");
		break;

	case 0x1004:
	case 0x1040 + VIRTIO_DEVICE_ID_SCSI:
		kprintf("VirtIO SCSI device (not implemented)\n");
		break;

	default:
		kfatal("Unknown VirtIO device ID %x\n",
		    [m_pciDevice configRead16:kDeviceID]);
	}
}

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
{
	if ((self = [super init])) {
		m_pciDevice = pciDevice;
		m_name = "virtio-pci-transport";
	}

	return self;
}

@end

static bool
intx_handler(md_intr_frame_t *, void *arg)
{
	VirtIOPCITransport *self = arg;
	uint8_t isr_status = *self->m_isr;

	if ((isr_status & 3) == 0)
		/* not for us */
		return false;

	ke_dpc_enqueue(&self->m_dpc);

	return true;
}

static void
dpc_handler(void *arg)
{
	VirtIOPCITransport *self = arg;
	[self deferredProcessing];
}
