/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file VirtIOPCITransport.m
 * @brief VirtIO transport for PCI
 */

#include <devicekit/DKAxis.h>
#include <devicekit/pci/DKPCIDevice.h>
#include <devicekit/virtio/DKVirtIOTransport.h>
#include <devicekit/virtio/virtio_pcireg.h>
#include <devicekit/virtio/virtioreg.h>
#include <sys/k_log.h>
#include <sys/k_intr.h>
#include <sys/kmem.h>
#include <sys/krx_endian.h>
#include <libkern/lib.h>

#include "devicekit/DKPlatformRoot.h"

@interface VirtIOPCITransport : DKVirtIOTransport <DKPCIDeviceMatching> {
    @public
	DKPCIDevice *m_pciDevice;
	DKDevice<DKVirtIODevice> *m_delegate;
	volatile struct virtio_pci_common_cfg *m_commonCfg;
	volatile uint8_t *m_isr;
	volatile void *m_devCfg;
	vaddr_t m_notify;
	size_t m_notifyOffMultiplier;
	kirq_t m_intxIrq;
	kdpc_t m_dpc;

	virtio_queue_t **m_queues;
	size_t m_queues_size;
}

@end

static bool intx_handler(void *);
static void dpc_handler(void *, void *);

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
}

- (int)enableDevice
{
	int r;
	kirq_source_t source;
	ipl_t ipl = IPL_HIGH;

	source = [m_pciDevice intxIrqSource];

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();

	r = [gPlatformRoot handleSource:&source
			    withHandler:intx_handler
			       argument:self
			     atPriority:&ipl
			      irqObject:&m_intxIrq];
	if (r < 0) {
		kdprintf("virtio: failed to allocate interrupt handler: %d\n",
		    r);
		return r;
	}

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();

	[m_pciDevice setInterrupts:true];

	return 0;
}

- (bool)exchangeFeaturesMandatory:(uint64_t)mandatory
			 optional:(uint64_t *)optional
{
	uint64_t negotiatedOptional = 0;

	for (int i = 0; i < 2; i++) {
		uint32_t mandatoryPart = mandatory >> (i * 32);
		uint32_t optionalPart = optional == NULL ?
		    0 :
		    (*optional >> (i * 32));
		uint32_t selectPart = mandatoryPart | optionalPart;
		uint32_t negotiatedPart;
		uint32_t deviceFeatures;

		m_commonCfg->device_feature_select = to_leu32(i);
		__sync_synchronize();

		deviceFeatures = from_leu32(m_commonCfg->device_feature);

		if ((deviceFeatures & mandatoryPart) != mandatoryPart) {
			kdprintf("Unsupported mandatory features (dword %d): "
				 "%x VS %x\n",
			    i, deviceFeatures, mandatoryPart);
			return false;
		}

		negotiatedPart = deviceFeatures & selectPart;
		negotiatedOptional |= ((uint64_t)negotiatedPart << (i * 32));

		m_commonCfg->guest_feature_select = to_leu32(i);
		__sync_synchronize();

		m_commonCfg->guest_feature = to_leu32(negotiatedPart);
		__sync_synchronize();
	}

	m_commonCfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
	__sync_synchronize();
	if (m_commonCfg->device_status !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		kdprintf("Features OK not set.\n");
		return false;
	}

	if (optional != NULL)
		*optional = negotiatedOptional;

	return true;
}

- (int)setupQueue:(virtio_queue_t *)queue index:(uint16_t)index
{
	vaddr_t addr;
	vaddr_t offs;

	queue->page = vm_page_alloc(VM_PAGE_DEV_BUFFER, 0, VM_DOMID_ANY,
	    VM_NOFAIL);
	addr = vm_page_hhdm_addr(queue->page);

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
	m_commonCfg->queue_desc = to_leu64(v2p((vaddr_t)queue->desc));
	m_commonCfg->queue_avail = to_leu64(v2p((vaddr_t)queue->avail));
	m_commonCfg->queue_used = to_leu64(v2p((vaddr_t)queue->used));
	m_commonCfg->queue_size = to_leu16(128);
	__sync_synchronize();

	m_commonCfg->queue_enable = to_leu16(1);

	__sync_synchronize();

	if (m_queues_size < index + 1) {
		m_queues_size = index + 1;
		m_queues = kmem_realloc(m_queues,
		    m_queues_size * sizeof(*m_queues),
		    (index + 1) * sizeof(*m_queues));
		m_queues[index] = queue;
	}

	return 0;
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
	kassert(ke_ipl() == IPL_DISP);

	for (size_t n = 0; n < m_queues_size; n++) {
		virtio_queue_t *queue = m_queues[n];
		uint16_t i;

		if (queue == NULL)
			continue;

		ke_spinlock_enter_nospl(&queue->spinlock);

		for (i = queue->last_seen_used;
		     i != from_leu16(queue->used->idx) % queue->length;
		     i = (i + 1) % queue->length) {
			volatile struct vring_used_elem *e =
			    &queue->used->ring[i % queue->length];
			[m_delegate processUsedDescriptor:e onQueue:queue];
		}

		queue->last_seen_used = i;

		[m_delegate additionalDeferredProcessingForQueue:queue];

		ke_spinlock_exit_nospl(&queue->spinlock);
	}
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
			kdprintf("VirtIO PCI capability: bar %d is not memory\n",
			    cap.bar);
			continue;
		}

		paddr = bar.base + from_leu32(cap.offset);
		kassert(paddr % PGSIZE == 0);

		/* FIXME: bad cache mode! */
		r = vm_k_map_phys(&vaddr, paddr,
		    roundup2(from_leu32(cap.length), PGSIZE) / PGSIZE, 0);
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
	[m_pciDevice setInterrupts:true];

	m_delegate = nil;

	switch ([m_pciDevice configRead16:kDeviceID]) {
	case 0x1000:
	case 0x1040 + VIRTIO_DEVICE_ID_NETWORK:
		kdprintf("Nic (not implemented)\n");
		break;

	case 0x1001:
	case 0x1040 + VIRTIO_DEVICE_ID_BLOCK:
		kdprintf("Block\n");
		break;

	case 0x1004:
	case 0x1040 + VIRTIO_DEVICE_ID_SCSI:
		kdprintf("VirtIO SCSI device (not implemented)\n");
		break;

	case 0x1009:
	case 0x1040 + VIRTIO_DEVICE_ID_9P:
		//m_delegate = [[VirtIO9pPort alloc] initWithTransport:self];
		kdprintf("9p (not implemented)\n");
		break;

	default:
		kfatal("Unknown VirtIO device ID %x\n",
		    [m_pciDevice configRead16:kDeviceID]);
	}

	if (m_delegate != nil) {
		[self attachChild:m_delegate onAxis:gDeviceAxis];
		[m_delegate addToStartQueue];
	}
}

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice
{
	if ((self = [super init])) {
		m_pciDevice = pciDevice;
		m_name = "virtio-pci-transport";
		ke_dpc_init(&m_dpc, dpc_handler, self, NULL);
	}

	return self;
}

@end

static bool
intx_handler(void * arg)
{
	VirtIOPCITransport *self = arg;
	uint8_t isr_status = *self->m_isr;

	if ((isr_status & 3) == 0)
		/* not for us */
		return false;

	ke_dpc_schedule(&self->m_dpc);

	return true;
}

static void
dpc_handler(void *arg, void *)
{
	VirtIOPCITransport *self = arg;
	[self deferredProcessing];
}
