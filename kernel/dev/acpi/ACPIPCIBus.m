/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#include "ddk/DKDevice.h"
#include "dev/acpi/ACPIPCIBus.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "uacpi/utilities.h"
#include "uacpi/resources.h"

#define PROVIDER ((DKACPIPlatform *)provider)

@implementation ACPIPCIBus

- (instancetype)initWithProvider:(DKACPIPlatform *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus
{
	self = [super initWithProvider:provider];
	acpiNode = node;
	m_seg = seg;
	m_bus = bus;

	kmem_asprintf(obj_name_ptr(self), "pcibus-%d-%d", seg, bus);

	[self registerDevice];
	DKLogAttach(self);

	[self enumerateDevices];

	return self;
}

- (dk_interrupt_source_t)routePCIPinForInfo:(struct pci_dev_info *)info
{
	uacpi_pci_routing_table *pci_routes;
	int r;
	dk_interrupt_source_t source;

	r=  uacpi_get_pci_routing_table(acpiNode, &pci_routes);
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

		source.edge = false;
		source.low_polarity = true;
		source.id = entry->index;

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
				source.id = irq->irqs[0];

				if (irq->triggering == UACPI_TRIGGERING_EDGE)
					source.edge = true;
				if (irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
					source.low_polarity = false;

				break;
			}
			case UACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
				uacpi_resource_extended_irq *irq =
				    &resource->extended_irq;

				kassert(irq->num_irqs >= 1);
				source.id = irq->irqs[0];

				if (irq->triggering == UACPI_TRIGGERING_EDGE)
					source.edge = true;
				if (irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
					source.low_polarity = false;

				break;
			}
			default:
				kfatal("unexpected\n");
			}

			uacpi_free_resources(resources);
		}
	}

	uacpi_free_pci_routing_table(pci_routes);

	return source;
}

@end
