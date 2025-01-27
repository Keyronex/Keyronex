/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Sun Aug 11 2024.
 */

#include <stdint.h>

#include "ddk/DKDevice.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/DKACPIPCIFirmwareInterface.h"
#include "dev/pci/DKPCIBus.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "uacpi/resources.h"
#include "uacpi/utilities.h"

#define PROVIDER ((DKACPIPlatform *)provider)

@implementation DKACPIPCIFirmwareInterface

@synthesize pciBus = m_PCIBus;

- (instancetype)initWithProvider:(DKDevice *)provider
			  parent:(DKACPIPCIFirmwareInterface *)parent
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus
		    upstreamSlot:(uint8_t)slot
		upstreamFunction:(uint8_t)fun
{
	self = [super init];
	if (self == nil)
		return nil;

	m_parent = parent;
	acpiNode = node;
	m_upstreamSlot = slot;
	m_upstreamFunction = fun;

	m_PCIBus = [DKPCIBus alloc];
	[m_PCIBus initWithProvider:provider
		 firmwareInterface:self
			       seg:seg
			       bus:bus];

	return self;
}

- (instancetype)initWithProvider:(DKDevice *)provider
			acpiNode:(uacpi_namespace_node *)node
			 segment:(uint16_t)seg
			     bus:(uint8_t)bus
{
	return [self initWithProvider:provider
			       parent:nil
			     acpiNode:node
			      segment:seg
				  bus:bus
			 upstreamSlot:0
		     upstreamFunction:0];
}

struct node_search {
	uint64_t adr;
	uacpi_namespace_node *node;
};

uacpi_iteration_decision
search_adr(uacpi_handle arg, uacpi_namespace_node *node, uacpi_u32)
{
	struct node_search *search = arg;
	uint64_t adr;
	uacpi_status r;

	r = uacpi_eval_adr(node, &adr);
	if (r != UACPI_STATUS_OK)
		return UACPI_ITERATION_DECISION_CONTINUE;

	if (adr == search->adr) {
		search->node = node;
		return UACPI_ITERATION_DECISION_BREAK;
	}

	return UACPI_ITERATION_DECISION_CONTINUE;
}

- (void)createDownstreamBus:(uint8_t)bus
		    segment:(uint16_t)segment
		   upstream:(DKPCIBus *)upstream
	       upstreamSlot:(uint8_t)slot
	   upstreamFunction:(uint8_t)fun;
{
	struct node_search search = { .adr = (slot << 16) | fun, .node = NULL };

	uacpi_namespace_for_each_child_simple(acpiNode, search_adr,
	    &search);

	[[DKACPIPCIFirmwareInterface alloc] initWithProvider:upstream
						      parent:self
						    acpiNode:search.node
						     segment:segment
							 bus:bus
						upstreamSlot:slot
					    upstreamFunction:fun];
}

- (int)routePCIPinForInfo:(struct pci_dev_info *)info
		     into:(out dk_interrupt_source_t *)source
{
	uacpi_pci_routing_table *pci_routes;
	int r = 0;

	if (acpiNode != NULL)
		r = uacpi_get_pci_routing_table(acpiNode, &pci_routes);

	if (acpiNode == NULL || r == UACPI_STATUS_NOT_FOUND) {
		struct pci_dev_info my_info = {
			.slot = m_upstreamSlot,
			.fun = m_upstreamFunction,
			.pin = ((info->pin - 1 + info->slot) % 4) + 1,
		};

		kassert(m_parent != nil);

		return [m_parent routePCIPinForInfo:&my_info into:source];
	}

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

		source->edge = false;
		source->low_polarity = true;
		source->id = entry->index;

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
				source->id = irq->irqs[0];

				if (irq->triggering == UACPI_TRIGGERING_EDGE)
					source->edge = true;
				if (irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
					source->low_polarity = false;

				break;
			}
			case UACPI_RESOURCE_TYPE_EXTENDED_IRQ: {
				uacpi_resource_extended_irq *irq =
				    &resource->extended_irq;

				kassert(irq->num_irqs >= 1);
				source->id = irq->irqs[0];

				if (irq->triggering == UACPI_TRIGGERING_EDGE)
					source->edge = true;
				if (irq->polarity == UACPI_POLARITY_ACTIVE_HIGH)
					source->low_polarity = false;

				break;
			}
			default:
				kfatal("unexpected\n");
			}

			uacpi_free_resources(resources);
		}
	}

	uacpi_free_pci_routing_table(pci_routes);

	kprintf("PCI pin %d routed to GSI %d\n", info->pin, source->id);

	return 0;
}

@end
