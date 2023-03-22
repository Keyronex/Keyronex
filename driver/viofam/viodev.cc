/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Feb 23 2023.
 */

#include "dev/virtio_pcireg.h"
#include "dev/virtioreg.h"
#include "kdk/kernel.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vm.h"

#include "../acpipc/ioapic.hh"
#include "viodev.hh"

#define le16_to_cpu(val) val

void
VirtIODevice::intrDpc(void *arg)
{
	VirtIODevice *dev = (VirtIODevice *)arg;
	dev->intrDpc();
}

/*! PCI Intx ISR. */
bool
VirtIODevice::intxISR(hl_intr_frame_t *frame, void *arg)
{
	VirtIODevice *dev = (VirtIODevice *)arg;
	uint8_t isr_status = *dev->isr;

#ifdef DEBUG_VIRTIO
	kdprintf("intx isr on %s\n", dev->objhdr.name);
#endif

	if ((isr_status & 3) == 0)
		/* not for us */
		return false;

	ke_dpc_enqueue(&dev->interrupt_dpc);

	return true;
}

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

	case VIRTIO_PCI_CAP_SHARED_MEMORY_CFG: {
		void *data = dev->device_cfg =
		    ((char *)P2V(PCIDevice::getBar(dev->pci_info, cap.bar)) +
			cap.offset);
		kdprintf("Shared Memory is at %p\n", data);
		break;
	}

	default:
		DKLog("VirtIODevice", "unknown capability config type %d\n",
		    cap.cfg_type);
	}
}

VirtIODevice::VirtIODevice(PCIDevice *provider, pci_device_info &info)
    : pci_info(info)
{
	PCIDevice::enableBusMastering(info);
	/*! need to disable this before playing with BARs */
	PCIDevice::enableMemorySpace(info, false);
	PCIDevice::enumerateCapabilities(info, enumerateCapabilitiesCallback,
	    this);
	PCIDevice::enableMemorySpace(info);

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
			DKDevLog(this,
			    "Unsupported features (dword %d): %x VS %x\n", i,
			    m_common_cfg->device_feature, requiredFeaturesPart);
			return false;
		}
		m_common_cfg->guest_feature_select = i;
		__sync_synchronize();
		m_common_cfg->guest_feature = requiredFeaturesPart;
		__sync_synchronize();
	}

	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK;
	__sync_synchronize();
	if (m_common_cfg->device_status !=
	    VIRTIO_CONFIG_DEVICE_STATUS_FEATURES_OK) {
		DKDevLog(this, "Features OK not set.\n");
		return false;
	}
	return true;
}

int
VirtIODevice::enableDevice()
{
	int r = 0;

	interrupt_dpc.callback = intrDpc;
	interrupt_dpc.arg = this;

	r = IOApic::handleGSI(pci_info.gsi, intxISR, this, pci_info.lopol,
	    pci_info.edge, kIPLDevice, &intx_entry);

	if (r < 0) {
		DKDevLog(this, "Failed to allocate interrupt handler: %d\n", r);
		return r;
	}

	m_common_cfg->device_status = VIRTIO_CONFIG_DEVICE_STATUS_DRIVER_OK;
	__sync_synchronize();

	PCIDevice::setInterrupts(pci_info, true);

	return 0;
}

int
VirtIODevice::setupQueue(virtio_queue *queue, uint16_t index)
{
	int r;
	vaddr_t addr;
	vaddr_t offs;

	r = vmp_page_alloc(kernel_process.map, true, kPageUseWired,
	    &queue->page);
	kassert(r == 0);
	addr = (vaddr_t)VM_PAGE_DIRECT_MAP_ADDR(queue->page);

	/* allocate a queue of total size 3336 bytes, to nicely fit in a page */
	queue->num = index;
	queue->length = 128;

	ke_spinlock_init(&queue->spinlock);

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
	queue->nfree_descs = 128;

	m_common_cfg->queue_select = index;
	__sync_synchronize();
	queue->notify_off = m_common_cfg->queue_notify_off;
	m_common_cfg->queue_desc = (uint64_t)V2P(queue->desc);
	m_common_cfg->queue_avail = (uint64_t)V2P(queue->avail);
	m_common_cfg->queue_used = (uint64_t)V2P(queue->used);
	m_common_cfg->queue_size = 128;
	__sync_synchronize();
	m_common_cfg->queue_enable = 1;

	__sync_synchronize();

	return 0;
}

void
VirtIODevice::notifyQueue(virtio_queue *queue)
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

	uint32_t *addr = (uint32_t *)(notify_base +
	    queue->notify_off * m_notify_off_multiplier);
	uint32_t value = queue->num;
	*addr = value;
	__sync_synchronize();
}

void
VirtIODevice::processVirtQueue(virtio_queue *queue)
{
	uint16_t i;

	for (i = queue->last_seen_used; i != queue->used->idx % queue->length;
	     i = (i + 1) % queue->length) {
		struct vring_used_elem *e =
		    &queue->used->ring[i % queue->length];
		processUsed(queue, e);
	}

	queue->last_seen_used = i;
}

uint16_t
VirtIODevice::allocateDescNumOnQueue(virtio_queue *queue)
{
	int r;

	r = queue->free_desc_index;
	kassert(r != queue->length);
	queue->free_desc_index = QUEUE_DESC_AT(queue, r).next;
	queue->nfree_descs--;

	return r;
}

void
VirtIODevice::freeDescNumOnQueue(virtio_queue *queue, uint16_t descNum)
{
	QUEUE_DESC_AT(queue, descNum).next = queue->free_desc_index;
	queue->free_desc_index = descNum;
	queue->nfree_descs++;
}

void
VirtIODevice::submitDescNumToQueue(virtio_queue *queue, uint16_t descNum)
{
	queue->avail->ring[queue->avail->idx % queue->length] = descNum;
	__sync_synchronize();
	queue->avail->idx += 1;
	__sync_synchronize();
}