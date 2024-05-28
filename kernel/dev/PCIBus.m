#include "PCIBus.h"
#include "dev/E1000.h"
#include "dev/virtio/DKVirtIOPCITransport.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "ntcompat/NTStorPort.h"

#if defined(__arch64__) || defined (__amd64__)
#include "uacpi/resources.h"
#include "uacpi/utilities.h"
#include "uacpi/kernel_api.h"

enum {
	kVendorID = 0x0, /* u16 */
	kDeviceID = 0x2, /* u16 */
	kCommand = 0x4,	 /* u16 */
	kStatus = 0x6,	 /* bit 4 = capabilities list exists */
	kSubclass = 0xa,
	kClass = 0xb,
	kHeaderType = 0xe, /* bit 7 = is multifunction */
	kBaseAddress0 = 0x10,
	kCapabilitiesPointer = 0x34,
	kInterruptPin = 0x3d, /* u8 */
};

enum {
	kCapMSIx = 0x11,
};

#define INFO_ARGS(INFO) (INFO)->seg, (INFO)->bus, (INFO)->slot, (INFO)->fun
#define ENABLE_CMD_FLAG(INFO, FLAG)           \
	pci_writew(INFO_ARGS(INFO), kCommand, \
	    pci_readw(INFO_ARGS(INFO), kCommand) | (FLAG))
#define DISABLE_CMD_FLAG(INFO, FLAG)          \
	pci_writew(INFO_ARGS(INFO), kCommand, \
	    pci_readw(INFO_ARGS(INFO), kCommand) & ~(FLAG))

@implementation PCIBus

+ (void)enableBusMasteringForInfo:(struct pci_dev_info *)info
{
	ENABLE_CMD_FLAG(info, 0x4);
}

+ (void)setMemorySpaceForInfo:(struct pci_dev_info *)info enabled:(BOOL)enabled
{
	if (enabled)
		DISABLE_CMD_FLAG(info, (1 << 10));
	else
		ENABLE_CMD_FLAG(info, (1 << 10));
}

+ (void)enumerateCapabilitiesForInfo:(struct pci_dev_info *)info
			    delegate:(DKDevice<DKPCIDeviceDelegate> *)delegate
{
	if (PCIINFO_CFG_READ(w, info, kStatus) & (1 << 4)) {
		voff_t capOffset = PCIINFO_CFG_READ(b, info,
		    kCapabilitiesPointer);

		while (capOffset != 0) {
			[delegate capabilityEnumeratedAtOffset:capOffset];
			capOffset = PCIINFO_CFG_READ(b, info, capOffset + 1);
		}
	}
}

+ (paddr_t)getBar:(int)num forInfo:(struct pci_dev_info *)info
{
	io_off_t off = kBaseAddress0 + sizeof(uint32_t) * num;
	uint32_t bar;

	uint64_t base;
	size_t len;

	bar = pci_readl(INFO_ARGS(info), off);

	if ((bar & 1) == 1) {
		kprintf("I/O space bar\n");
		return 0;
	} else if (((bar >> 1) & 3) == 0) {
		uint32_t size_mask;

		pci_writel(INFO_ARGS(info), off, 0xffffffff);
		size_mask = pci_readl(INFO_ARGS(info), off);
		pci_writel(INFO_ARGS(info), off, bar);

		base = bar & 0xffffffF0;
		len = (size_t)1 << __builtin_ctzl(size_mask & 0xffffffF0);

	} else {
		uint64_t size_mask, bar_high, size_mask_high;

		kassert(((bar >> 1) & 3) == 2);

		bar_high = pci_readl(INFO_ARGS(info), off + 4);
		base = (bar & 0xffffffF0) | (bar_high << 32);

		pci_writel(INFO_ARGS(info), off, 0xffffffff);
		pci_writel(INFO_ARGS(info), off + 4, 0xffffffff);
		size_mask = pci_readl(INFO_ARGS(info), off);
		size_mask_high = pci_readl(INFO_ARGS(info), off + 4);
		pci_writel(INFO_ARGS(info), off, bar);
		pci_writel(INFO_ARGS(info), off + 4, bar_high);

		size_mask |= size_mask_high << 32;
		len = (size_t)1
		    << __builtin_ctzl(size_mask & 0xffffffffffffffF0);

		(void)len;
	}

	return (paddr_t)base;
}

+ (void)setInterruptsEnabled:(BOOL)enabled forInfo:(struct pci_dev_info *)info
{
	if (enabled)
		DISABLE_CMD_FLAG(info, (1 << 10));
	else
		ENABLE_CMD_FLAG(info, (1 << 10));
}

- (int)routePinForInfo:(struct pci_dev_info *)info
{
	uacpi_pci_routing_table *pci_routes;
	int r = uacpi_get_pci_routing_table(acpiNode, &pci_routes);

	kassert(r == UACPI_STATUS_OK);

	for (size_t i = 0; i < pci_routes->num_entries; i++) {
		uacpi_pci_routing_table_entry *entry;
		int entry_slot, entry_fun;

		entry = &pci_routes->entries[i];
		entry_slot = (entry->address >> 16) & 0xffff;
		entry_fun = entry->address & 0xffff;

		if (entry->pin != info->pin - 1 || entry_slot != info->slot ||
		    (entry_fun != 0xffff && entry_fun != info->fun))
			continue;

		info->edge = false;
		info->lopol = true;
		info->gsi = entry->index;

		if (entry->source != NULL) {
			uacpi_resources *resources;
			uacpi_resource *resource;
			r = uacpi_get_current_resources(entry->source,
			    &resources);
			kassert(r == UACPI_STATUS_OK);

			resource = &resources->entries[0];

			switch (resource->type) {
			case UACPI_RESOURCE_TYPE_IRQ: {
				uacpi_resource_irq *irq = &resource->irq;

				kassert(irq->num_irqs >= 1);
				info->gsi = irq->irqs[0];

				if (irq->triggering == UACPI_TRIGGERING_EDGE)
					info->edge = true;
				if (irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
					info->lopol = false;

				break;
			}
			case UACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
				uacpi_resource_extended_irq *irq =
				    &resource->extended_irq;

				kassert(irq->num_irqs >= 1);
				info->gsi = irq->irqs[0];

				if (irq->triggering == UACPI_TRIGGERING_EDGE)
					info->edge = true;
				if (irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
					info->lopol = false;

				break;
			}
			default:
				kfatal("unexpected\n");
			}

			uacpi_free_resources(resources);
		}
	}

	uacpi_free_pci_routing_table(pci_routes);

	return 0;
}

static BOOL
match_e1000(uint16_t vendorId, uint16_t devId)
{
	if (vendorId != 0x8086)
		return NO;

	switch (devId) {
	case 0x100c: /* 82544GC */
	case 0x100e: /* 82540EM */
	case 0x100f: /* 82545EM */
	case 0x10d3: /* 82574L */
	case 0x15bd: /* I219-LM */
	case 0x15d7: /* I219-LM */
	case 0x15fa: /* I219-V */
		return YES;

	default:
		return NO;
	}
}

- (void)doSegment:(uint16_t)seg
	      bus:(uint8_t)bus
	     slot:(uint8_t)slot
	 function:(uint8_t)fun
{
	struct pci_dev_info info;

#if defined(__arch64__) || defined (__amd64__)
#define CFG_READ(WIDTH, OFFSET) \
	pci_read##WIDTH(seg, bus, slot, fun, OFFSET)

	info.vendorId = CFG_READ(w, kVendorID);
	if (info.vendorId == 0xffff)
		return;
	info.deviceId = CFG_READ(w, kDeviceID);
	info.klass = CFG_READ(b, kClass);
	info.subClass = CFG_READ(b, kSubclass);

	info.seg = seg;
	info.bus = bus;
	info.slot = slot;
	info.fun = fun;
	info.pin = CFG_READ(b, kInterruptPin);

	if (info.pin != 0) {
		int r = [self routePinForInfo:&info];
		kassert(r == 0);
	}

#endif

#if 0
	kprintf("%x:%x (klass %x subclass %x)\n", info.vendorId, info.deviceId,
	    info.klass, info.subClass);
#endif

	BOOL handled;
	handled = [NTStorPort probeWithPCIBus:self info:&info];
	if (!handled && info.vendorId == 0x1af4)
		handled = [DKVirtIOPCITransport probeWithPCIBus:self
							   info:&info];
	else if (!handled && match_e1000(info.vendorId, info.deviceId))
		handled = [E1000 probeWithPCIBus:self info:&info];
	if (!handled)
		kprintf("No driver for PCI device %d.%d.%d (%x:%x)\n", info.bus,
		    info.slot, info.fun, info.vendorId, info.deviceId);
}

- (instancetype)initWithProvider:(DKDevice *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus

{
	self = [super initWithProvider:provider];
	acpiNode = node;

	kmem_asprintf(obj_name_ptr(self), "pcibus-%d-%d", seg, bus);

	[self registerDevice];
	DKLogAttach(self);

#if defined(__arch64__) || defined (__amd64__)
	for (unsigned slot = 0; slot < 32; slot++) {
		size_t nFun = 1;

		if (pci_readw(seg, bus, slot, 0, kVendorID) == 0xffff)
			continue;

		if (pci_readb(seg, bus, slot, 0, kHeaderType) &
		    (1 << 7))
			nFun = 8;

		for (unsigned fun = 0; fun < nFun; fun++)
			[self doSegment:seg bus:bus slot:slot function:fun];
	}
#endif

	return self;
}

@end

#endif
