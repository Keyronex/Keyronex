#include "PCIBus.h"
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

@implementation PCIBus

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

	[NTStorPort probeWithPCIBus:self info:&info];

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
