/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file DKACPIPlatformRoot.m
 * @brief ACPI platform root device implementation.
 */

#include <sys/k_log.h>

#include <devicekit/acpi/DKACPINode.h>
#include <devicekit/acpi/DKACPIPlatformRoot.h>
#include <devicekit/pci/DKPCIBridge.h>
#include <devicekit/DKAxis.h>
#include <devicekit/DKPlatformRoot.h>

#include <uacpi/acpi.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

@interface DKACPINode ()
+ (void)drainStartDevicesQueue;
- (void)addToStartDevicesQueue;
- (void)startDevices;
@end

void DKLogAttach(DKDevice *child, DKDevice *parent);

DKAxis *gACPIAxis;

@implementation DKACPIPlatform

+ (instancetype)root
{
	return (DKACPIPlatform *)gPlatformRoot;
}

- (instancetype)init
{
	kassert(uacpi_initialize(0) == UACPI_STATUS_OK);
	kassert(uacpi_namespace_load() == UACPI_STATUS_OK);
	kassert(uacpi_namespace_initialize() == UACPI_STATUS_OK);
	kassert(uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC) ==
	    UACPI_STATUS_OK);

	self = [super initWithNamespaceNode:uacpi_namespace_root()];
	if (self != nil) {
		m_nsNode = uacpi_namespace_root();

		gACPIAxis = [DKAxis axisWithName:"DKACPI"];
		[gACPIAxis addChild:self ofParent:nil];
		DKLogAttach(self, nil);
	}
	return self;
}

/* ACPI node -> PCI root -> PCI device -> PCI Bridge -> PCI Device -> etc */
- (void)routePCIPin:(uint8_t)pin
	  forBridge:(DKPCIBridge *)bridge
	       slot:(uint8_t)slot
	   function:(uint8_t)fun
	       into:(out kirq_source_t *)source
{
	uacpi_pci_routing_table *prt = NULL;
	int r = 0;

	while (bridge != NULL) {
		DKACPINode *promNode = (DKACPINode *)bridge.promNode;

		if (promNode) {
			r = uacpi_get_pci_routing_table(promNode->m_nsNode,
			    &prt);
			if (r == UACPI_STATUS_OK) {
				kdprintf("prt is %p\n", prt);
				break;
			}
		}

		if (promNode == nil || r == UACPI_STATUS_NOT_FOUND) {
			DKPCI2PCIBridge *pci2pci;

			kassert([bridge isKindOfClass:[DKPCI2PCIBridge class]]);

			pci2pci = (DKPCI2PCIBridge *)bridge;

			/* follow default expansion bridge routing */
			pin = ((pin - 1 + slot) % 4) + 1;
			slot = pci2pci.pciDevice.address.slot;
			fun = pci2pci.pciDevice.address.function;

			bridge = bridge.parentBridge;
			continue;
		}

		/* routed by PRT */
		kassert(r == UACPI_STATUS_OK);
		break;
	}

	for (size_t i = 0; i < prt->num_entries; i++) {
		uacpi_pci_routing_table_entry *entry;
		int entry_slot, entry_fun;

		entry = &prt->entries[i];
		entry_slot = (entry->address >> 16) & 0xffff;
		entry_fun = entry->address & 0xffff;

		if (entry->pin != pin - 1 || entry_slot != slot ||
		    (entry_fun != 0xffff && entry_fun != fun))
			continue;

		/* default edge false, lopol true */
		source->source = entry->index;
		source->edge = false;
		source->low_polarity = true;

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
				source->source = irq->irqs[0];

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
				source->source = irq->irqs[0];

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

	uacpi_free_pci_routing_table(prt);
}

- (int)handleSource:(struct kirq_source *)source
	withHandler:(kirq_handler_t *)handler
	   argument:(void *)arg
	 atPriority:(ipl_t *)ipl
	  irqObject:(out kirq_t *)object;
{
	kfatal("subclass responsibility");
}

- (void)handleMADTEntry:(struct acpi_entry_hdr *)item
{
	kfatal("subclass responsibility");
}

- (void)iterateMADT
{
	struct acpi_entry_hdr *entry;
	struct acpi_madt *madt;
	uint8_t *madt_lim;
	uacpi_table madt_table;
	int r;

	r = uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &madt_table);
	if (r != UACPI_STATUS_OK)
		kfatal("Failed to find MADT: %d\n", r);

	madt = madt_table.ptr;
	madt_lim = ((uint8_t *)madt) + madt->hdr.length;

	for (uint8_t *elem = (uint8_t *)madt->entries; elem < madt_lim;
	    elem += entry->length) {
		entry = (struct acpi_entry_hdr *)elem;
		[self handleMADTEntry:entry];
	}

	uacpi_table_unref(&madt_table);
}

- (void)start
{
	[super start];
	[DKDevice drainStartQueue];
	[self iterateMADT];
	[self startDevices];
	[DKACPINode drainStartDevicesQueue];
	[DKDevice drainStartQueue];
}

@end

void
dk_acpi_presmp_init(void)
{
	gPlatformRoot = [[DKACPIPlatform alloc] init];
}

void
dk_acpi_threaded_init(void)
{
	[gPlatformRoot start];
	[gDeviceAxis printSubtreeOfDevice:gPlatformRoot];
}
