#include "ddk/DKAxis.h"
#include "ddk/DKDevice.h"
#include "ddk/DKUtilities.h"
#include "ddk/virtio_mmio.h"
#include "ddk/virtioreg.h"
#include "dev/virtio/VirtIO9pPort.h"
#include "dev/virtio/VirtIOGPU.h"
#include "dev/virtio/VirtIOMMIOTransport.h"
#include "kdk/endian.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "vm/vmp.h"

#define MMIO_READ32(BASE, REG) \
	le32_to_native(*(volatile uint32_t *)(BASE + REG))
#define MMIO_WRITE32(BASE, REG, VAL) \
	*(volatile uint32_t *)(BASE + REG) = le32_to_native(VAL)

void gfpic_handle_irq(unsigned int vector,
    bool (*handler)(md_intr_frame_t *, void *), void *arg);
void gfpic_mask_irq(unsigned int vector);
void gfpic_unmask_irq(unsigned int vector);

@interface DKVirtIOMMIOTransport (Private)
- (instancetype)initWithProvider:(DKDevice *)provider
			    mmio:(volatile void *)mmio
		       interrupt:(int)interrupt;
@end

static bool int_handler(md_intr_frame_t *, void *);
static void dpc_handler(void *);

static unsigned counter = 0;

static const char *device_names[] = { NULL, "network card", "block device",
	"console", "entropy source", "memory ballooning (traditional)",
	"ioMemory", "rpmsg", "SCSI host", "9P transport", "mac80211 wlan",
	"rproc serial", "virtio CAIF", "memory balloon", NULL, NULL,
	"GPU device", "Timer/Clock device", "Input device", "Socket device",
	"Crypto device", "Signal Distribution Module", "pstore device",
	"IOMMU device", "Memory device", "Audio device", "file system device",
	"PMEM device", "RPMB device",
	"mac80211 hwsim wireless simulation device", "Video encoder device",
	"Video decoder device", "SCMI device", "NitroSecureModule",
	"I2C adapter", "Watchdog", "CAN device", NULL, "Parameter Server",
	"Audio policy device", "Bluetooth device", "GPIO device",
	"RDMA device" };

static const char *
device_name(uint32_t id)
{
	const char *name;

	if (id >= elementsof(device_names))
		return "Unknown device";

	name = device_names[id];
	return name ? name : "Unknown device";
}

@implementation DKVirtIOMMIOTransport

+ (instancetype)probeWithMMIO:(volatile void *)mmio
		interrupt:(int)interrupt
{
	if (MMIO_READ32(mmio, VIRTIO_MMIO_DEVICE_ID) != 0)
		return [[self alloc] initWithMMIO:mmio
				     interrupt:interrupt];
	return nil;
}

- (instancetype)initWithMMIO:(volatile void *)mmio
		       interrupt:(int)interrupt
{
	uint32_t device_id;

	self = [super init];
	m_mmio = mmio;
	m_interrupt = interrupt;
	kmem_asprintf(&m_name, "virtio-mmio-%u", counter++);

	m_dpc.arg = self;
	m_dpc.callback = dpc_handler;
	m_dpc.cpu = NULL;

	device_id = MMIO_READ32(mmio, VIRTIO_MMIO_DEVICE_ID);

	switch (device_id) {
	case VIRTIO_DEVICE_ID_9P:
		m_delegate = [[VirtIO9pPort alloc] initWithTransport:self];
		break;

	case VIRTIO_DEVICE_ID_GPU:
		m_delegate = [[VirtIOGPU alloc] initWithTransport:self];
		break;

	default:
		kprintf("No driver for this device\n");
	}

	if (m_delegate != nil) {
		[self attachChild:m_delegate onAxis:gDeviceAxis];
		[m_delegate addToStartQueue];
	}

	return self;
}

- (volatile void *)deviceConfig
{
	return m_mmio + VIRTIO_MMIO_CONFIG;
}

- (void)enqueueDPC
{
	ke_dpc_enqueue(&self->m_dpc);
}

- (void)resetDevice
{
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_STATUS,
	    VIRTIO_CONFIG_DEVICE_STATUS_RESET);
	__sync_synchronize();
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_STATUS,
	    VIRTIO_CONFIG_DEVICE_STATUS_ACK);
	__sync_synchronize();
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_STATUS,
	    VIRTIO_CONFIG_DEVICE_STATUS_DRIVER);
	__sync_synchronize();
}

- (bool)exchangeFeaturesMandatory:(uint64_t)mandatory
			 optional:(uint64_t *)optional
{
	uint64_t negotiatedOptional = 0;

	for (int i = 0; i < 2; i++) {
		uint32_t mandatoryFeaturesPart = mandatory >> (i * 32);
		uint32_t optionalFeaturesPart = (optional == NULL ?
			0 :
			(*optional >> (i * 32)));
		uint32_t requiredFeaturesPart = mandatoryFeaturesPart |
		    optionalFeaturesPart;
		uint32_t deviceFeaturesPart;
		uint32_t negotiatedFeaturesPart;

		MMIO_WRITE32(m_mmio, VIRTIO_MMIO_DEVICE_FEATURES_SEL, i);
		__sync_synchronize();
		deviceFeaturesPart = MMIO_READ32(m_mmio,
		    VIRTIO_MMIO_DEVICE_FEATURES);

		if ((deviceFeaturesPart & mandatoryFeaturesPart) !=
		    mandatoryFeaturesPart) {
			DKDevLog(self,
			    "Unsupported mandatory features in dword %d:"
			    "\n\tDevice has 0x%x"
			    "\n\tMandatory required 0x%x\n",
			    i, deviceFeaturesPart, mandatoryFeaturesPart);
			return false;
		}

		negotiatedFeaturesPart = deviceFeaturesPart &
		    requiredFeaturesPart;
		negotiatedOptional |= ((uint64_t)negotiatedFeaturesPart
		    << (i * 32));

		MMIO_WRITE32(m_mmio, VIRTIO_MMIO_DRIVER_FEATURES_SEL, i);
		__sync_synchronize();
		MMIO_WRITE32(m_mmio, VIRTIO_MMIO_DRIVER_FEATURES,
		    negotiatedFeaturesPart);
		__sync_synchronize();
	}

	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_STATUS,
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK);
	__sync_synchronize();

	if (MMIO_READ32(m_mmio, VIRTIO_MMIO_STATUS) !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		DKDevLog(self, "Features OK not set.\n");
		return false;
	}

	if (optional != NULL) {
		*optional = negotiatedOptional;
	}

	return true;
}

- (int)setupQueue:(struct virtio_queue *)queue index:(uint16_t)index
{
	int r;
	vaddr_t addr;
	vaddr_t offs;

	r = vm_page_alloc(&queue->page, 0,  kPageUseKWired,
	    true);
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
		queue->desc[i].next = to_leu16(i + 1);
	queue->free_desc_index = 0;
	queue->nfree_descs = 128;

	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_SEL, index);
	__sync_synchronize();

	/* apparently we don't convert these to LE32? */
#define TO_LE32(X) (X)
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_DESC_LOW,
	    TO_LE32((uint32_t)V2P(queue->desc)));
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_AVAIL_LOW,
	    TO_LE32((uint32_t)V2P(queue->avail)));
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_USED_LOW,
	    TO_LE32((uint32_t)V2P(queue->used)));
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_NUM, 128);

	__sync_synchronize();
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_READY, 1);

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

- (int)enableDevice
{
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_STATUS,
	    VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK);
	__sync_synchronize();

	if (MMIO_READ32(m_mmio, VIRTIO_MMIO_STATUS) !=
	    VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK) {
		return -1;
	}

#ifdef __m68k__
	gfpic_handle_irq(m_interrupt, int_handler, self);
	gfpic_unmask_irq(m_interrupt);
#else
	(void)int_handler;
#endif
	return 0;
}

- (int)allocateDescNumOnQueue:(struct virtio_queue *)queue
{
	int r;

	r = queue->free_desc_index;
	kassert(r < queue->length);
	queue->free_desc_index = from_leu16(QUEUE_DESC_AT(queue, r).next);
	kassert(queue->free_desc_index < queue->length);
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
	kassert(descNum <= 64);
	kprintf("Current index: %u\n Writing New Index: %u\n",
	    from_leu16(queue->avail->idx) % queue->length,
	    from_leu16(queue->avail->idx) + 1);
#endif
	queue->avail->ring[from_leu16(queue->avail->idx) % queue->length] =
	    to_leu16(descNum);
	__sync_synchronize();
	queue->avail->idx = to_leu16(from_leu16(queue->avail->idx) + 1);
	__sync_synchronize();
}

- (void)notifyQueue:(struct virtio_queue *)queue
{
	MMIO_WRITE32(m_mmio, VIRTIO_MMIO_QUEUE_NOTIFY,
	    native_to_le16(queue->index));
}

- (void)deferredProcessing
{
	kassert(splget() == kIPLDPC);

	for (size_t n = 0; n < m_queues_size; n++) {
		virtio_queue_t *queue = m_queues[n];
		uint16_t i;

		if (queue == NULL)
			continue;

		ke_spinlock_acquire_nospl(&queue->spinlock);

		for (i = queue->last_seen_used;
		     i != from_leu16(queue->used->idx) % queue->length;
		     i = (i + 1) % queue->length) {
			volatile struct vring_used_elem *e =
			    &queue->used->ring[i % queue->length];
			[m_delegate processUsedDescriptor:e onQueue:queue];
		}

		queue->last_seen_used = i;

		[m_delegate additionalDeferredProcessingForQueue:queue];

		ke_spinlock_release_nospl(&queue->spinlock);
	}
}

@end


static bool
int_handler(md_intr_frame_t *, void *arg)
{
	DKVirtIOMMIOTransport *self = arg;
	uint32_t stat = MMIO_READ32(self->m_mmio, VIRTIO_MMIO_INTERRUPT_STATUS);
#if DEBUG_VIRTIO >= 2
	DKDevLog(self, "virtio-mmio interrupt (status 0x%x)\n", stat);
#endif
	MMIO_WRITE32(self->m_mmio, VIRTIO_MMIO_INTERRUPT_ACK, stat);
	ke_dpc_enqueue(&self->m_dpc);
	return true;
}

static void
dpc_handler(void *arg)
{
	DKVirtIOMMIOTransport *self = arg;
	[self deferredProcessing];
}
