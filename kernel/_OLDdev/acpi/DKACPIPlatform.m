#include <limine.h>
#include <string.h>

#include "dev/pci/DKPCIBus.h"
#include "dev/acpi/DKACPIPCIFirmwareInterface.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/tables.h"
#include "kdk/executive.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vm.h"
#include "uacpi/event.h"
#include "uacpi/internal/namespace.h"
#include "uacpi/kernel_api.h"
#include "uacpi/namespace.h"
#include "uacpi/status.h"
#include "uacpi/tables.h"
#include "uacpi/types.h"
#include "uacpi/uacpi.h"
#include "uacpi/utilities.h"

#ifdef __amd64
#include "dev/PS2Keyboard.h"
#endif

struct pcie_ecam {
	paddr_t phys;
	vaddr_t virt;
	uint64_t seg;
	uint8_t start_bus, end_bus;
};

static DKACPIPlatform *acpiplatform = NULL;
struct pcie_ecam *ecam;

extern vaddr_t rsdp_address;

#define ACPI_PCI_ROOT_BUS_PNP_ID "PNP0A03"
#define ACPI_PCIE_ROOT_BUS_PNP_ID "PNP0A08"

@implementation DKACPIPlatform

- (void)makePCIBusFromNode:(uacpi_namespace_node *)node
{
	uint64_t seg = 0, bus = 0;
	DKPCIBus *pcibus;
	int r;

	r = uacpi_eval_integer(node, "_SEG", NULL, &seg);
	if (r != UACPI_STATUS_OK && r != UACPI_STATUS_NOT_FOUND) {
		DKDevLog(self, "failed to evaluate _SEG: %d\n", r);
		return;
	}

	r = uacpi_eval_integer(node, "_BBN", NULL, &bus);
	if (r != UACPI_STATUS_OK && r != UACPI_STATUS_NOT_FOUND) {
		DKDevLog(self, "failed to evaluate _BBN: %d\n", r);
		return;
	}

	pcibus = [[DKACPIPCIFirmwareInterface alloc] initWithProvider:self
							     acpiNode:node
							      segment:seg
								  bus:bus];
	(void)pcibus;
}

static uacpi_iteration_decision
iteration_callback(void *user, uacpi_namespace_node *node, uacpi_u32)
{
	const char *pci_list[] = { ACPI_PCI_ROOT_BUS_PNP_ID,
		ACPI_PCIE_ROOT_BUS_PNP_ID, NULL };
	DKACPIPlatform *self = user;

	if (uacpi_device_matches_pnp_id(node, pci_list))
		[self makePCIBusFromNode:node];
#ifdef __amd64
	else if (uacpi_device_matches_pnp_id(node,
		     (const char *const[]) { "PNP0303", "PNP030B", "PNP0320",
			 NULL }))
		[PS2Keyboard probeWithProvider:self acpiNode:node];
#endif

	return UACPI_ITERATION_DECISION_CONTINUE;
}

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

static void
mcfg_walk(void)
{
	uacpi_table table;
	struct acpi_mcfg *mcfg;
	size_t nentries;
	int r;

	r = uacpi_table_find_by_signature("MCFG", &table);
	if (r != 0) {
		DKDevLog(acpiplatform, "No mcfg table found.\n");
		return;
	}

	mcfg = (void *)table.virt_addr;
	nentries = (mcfg->hdr.length - sizeof(struct acpi_mcfg)) /
	    sizeof(struct acpi_mcfg_allocation);
	kassert(nentries == 1); /* todo(low): handle multiple */
	for (int i = 0; i < nentries; i++) {
		struct acpi_mcfg_allocation *entry = &mcfg->entries[i];
		DKDevLog(acpiplatform, "PCI-E eCAM base [%d:%d-%d]: 0x%lx\n",
		    entry->segment, entry->start_bus, entry->end_bus,
		    entry->address);
		[DKPCIBus setECAMBase:entry->address];
	}
}

- (instancetype)initWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	int r;

	self = [super initWithProvider:provider];
	kmem_asprintf(obj_name_ptr(self), "acpi-platform");
	acpiplatform = self;
	[self registerDevice];
	DKLogAttach(self);

	r = uacpi_initialize(0);
	kassert(r == UACPI_STATUS_OK);

	mcfg_walk();

	[self iterateArchSpecificEarlyTables];

	return self;
}

- (void)secondStageInit
{
	uacpi_namespace_node *sb;
	int r;

	DKDevLog(self, "ACPI second-stage init\n");

	r = uacpi_namespace_load();
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_namespace_initialize();
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
	kassert(r == UACPI_STATUS_OK);

	sb = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
	kassert(sb != NULL);


	uacpi_namespace_for_each_child_simple(sb, iteration_callback, self);
}

- (void)iterateArchSpecificEarlyTables
{
	kfatal("Subclass responsibility\n");
}

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	rsdp_address = rsdp;
	[[self alloc] initWithProvider:provider rsdp:rsdp];

	return YES;
}

+ (instancetype)instance
{
	return acpiplatform;
}

@end
