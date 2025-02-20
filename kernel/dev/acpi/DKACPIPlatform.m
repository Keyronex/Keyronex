/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Wed Apr 17 2024.
 */

#include <ddk/DKAxis.h>
#include <ddk/DKInterrupt.h>
#include <ddk/DKPlatformRoot.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <limine.h>
#include <uacpi/acpi.h>
#include <uacpi/resources.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

#include "dev/SimpleFB.h"
#include "dev/acpi/DKACPIPlatform.h"
#include "dev/pci/DKPCIBridge.h"
#include "dev/pci/ecam.h"

extern volatile struct limine_framebuffer_request fb_request;

void DKLogAttach(DKDevice *child, DKDevice *parent);

DKAxis *gACPIAxis;
DKACPIPlatform *gACPIPlatform;

@implementation DKACPIPlatform

+ (instancetype)root
{
	return gACPIPlatform;
}

- (void)iterateArchSpecificEarlyTables
{
	kfatal("Method must be overridden by platform-specific category.\n");
}

- (void)setupEarlyTableAccess
{
	int r;
	static char acpi_early_table_storage[4096];

	r = uacpi_setup_early_table_access(&acpi_early_table_storage,
	    sizeof(acpi_early_table_storage));
	if (r != UACPI_STATUS_OK)
		kfatal("Can't set up early table access\n");
}

- (void)enumerateMcfg
{
	struct uacpi_table mcfg_table;
	struct acpi_mcfg *mcfg;
	struct acpi_mcfg_allocation *alloc;

	if (uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &mcfg_table) !=
	    UACPI_STATUS_OK)
		kfatal("No MCFG table found\n");

	mcfg = (void *)mcfg_table.virt_addr;

	/* Calculate number of entries based on table length */
	ecam_spans_n = (mcfg->hdr.length - sizeof(struct acpi_mcfg)) /
	    sizeof(struct acpi_mcfg_allocation);

	ecam_spans = kmem_alloc(sizeof(struct ecam_span) * ecam_spans_n);

	/* Iterate through each MCFG allocation entry */
	for (size_t i = 0; i < ecam_spans_n; i++) {
		alloc = &mcfg->entries[i];
		ecam_spans[i].base = alloc->address;
		ecam_spans[i].seg = alloc->segment;
		ecam_spans[i].bus_start = alloc->start_bus;
		ecam_spans[i].bus_end = alloc->end_bus;
	}

	uacpi_table_unref(&mcfg_table);
}

- (instancetype)init
{
	int r;
	struct limine_framebuffer *fb;
	SimpleFB *bootFb;

	[self setupEarlyTableAccess];
	[self enumerateMcfg];

	fb =  fb_request.response->framebuffers[0];
	bootFb = [[SimpleFB alloc] initWithAddress:V2P(fb->address)
					     width:fb->width
					    height:fb->height
					     pitch:fb->pitch];
	[bootFb start];

	gACPIAxis = [DKAxis axisWithName:"DKACPI"];

	r = uacpi_initialize(0);
	kassert(r == UACPI_STATUS_OK);

	[gACPIAxis addChild:self ofParent:nil];
	DKLogAttach(self, nil);

	gACPIPlatform = self;
	gPlatformRoot = self;

	[self attachChild:bootFb onAxis:gDeviceAxis];

	[self iterateArchSpecificEarlyTables];

	return self;
}

- (void)start
{
	int r;

	r = uacpi_namespace_load();
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_namespace_initialize();
	kassert(r == UACPI_STATUS_OK);

	r = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
	kassert(r == UACPI_STATUS_OK);

	self = [super initWithNamespaceNode:uacpi_namespace_root()];

	[super start];
	[DKDevice drainStartQueue];
	[self startDevices];
	[DKACPINode drainStartDevicesQueue];
}

/* ACPI node -> PCI root -> PCI device -> PCI Bridge -> PCI Device -> etc */
- (void)routePCIPin:(uint8_t)pin
	  forBridge:(DKPCIBridge *)bridge
	       slot:(uint8_t)slot
	   function:(uint8_t)fun
	       into:(out dk_interrupt_source_t *)source
{
	uacpi_pci_routing_table *prt = NULL;
	int r = 0;

	while (bridge != NULL) {
		DKACPINode *promNode = (DKACPINode *)bridge.promNode;

		if (promNode) {
			r = uacpi_get_pci_routing_table(promNode->m_nsNode,
			    &prt);
			if (r == UACPI_STATUS_OK) {
				kprintf("prt is %p\n", prt);
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
		source->id = entry->index;
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

	uacpi_free_pci_routing_table(prt);
}

- (int)allocateLeastLoadedMSIInterruptForEntries:(struct intr_entry *)entries
					   count:(size_t)count
				     msiAddress:(out uint32_t *)msiAddress
					msiData:(out uint32_t *)msiData
{
	kfatal("Method must be overridden by platform-specific category.\n");
}

- (int)allocateLeastLoadedMSIxInterruptForEntry:(struct intr_entry *)entry
				    msixAddress:(out uint32_t *)msixAddress
				       msixData:(out uint32_t *)msixData
{
	kfatal("Method must be overridden by platform-specific category.\n");
}

@end

void
dk_acpi_madt_walk(struct acpi_madt *madt,
    void (*callback)(struct acpi_entry_hdr *item, void *arg), void *arg)
{
	for (char *item = (char *)&madt->entries[0];
	     item < ((char *)madt->entries +
			(madt->hdr.length - sizeof(struct acpi_madt)));) {
		struct acpi_entry_hdr *header = (struct acpi_entry_hdr *)item;
		callback(header, arg);
		item += header->length;
	}
}
