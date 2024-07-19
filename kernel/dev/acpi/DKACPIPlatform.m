#include <limine.h>
#include <string.h>

#include "dev/PCIBus.h"
#include "dev/PS2Keyboard.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/tables.h"
#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "uacpi/event.h"
#include "uacpi/internal/namespace.h"
#include "uacpi/kernel_api.h"
#include "uacpi/namespace.h"
#include "uacpi/status.h"
#include "uacpi/tables.h"
#include "uacpi/types.h"
#include "uacpi/uacpi.h"
#include "uacpi/utilities.h"

#ifdef AMD64
#include "dev/amd64/IOAPIC.h"
#include "kdk/amd64/portio.h"
#endif

struct pcie_ecam {
	paddr_t phys;
	vaddr_t virt;
	uint64_t seg;
	uint8_t start_bus, end_bus;
};

acpi_rsdt_t *rsdt = NULL;
acpi_xsdt_t *xsdt = NULL;
mcfg_t *mcfg = NULL;
static DKACPIPlatform *acpiplatform = NULL;
struct pcie_ecam *ecam;

#define ACPI_PCI_ROOT_BUS_PNP_ID "PNP0A03"
#define ACPI_PCIE_ROOT_BUS_PNP_ID "PNP0A08"

@implementation DKACPIPlatform

- (void)makePCIBusFromNode:(uacpi_namespace_node *)node
{
	uint64_t seg = 0, bus = 0;
	PCIBus *pcibus;
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

	pcibus = [[PCIBus alloc] initWithProvider:self acpiNode:node segment:seg bus:bus];
	(void)pcibus;
}

static uacpi_ns_iteration_decision
iteration_callback(void *user, uacpi_namespace_node *node)
{
	const char *pci_list[] = { ACPI_PCI_ROOT_BUS_PNP_ID,
		ACPI_PCIE_ROOT_BUS_PNP_ID, NULL };
	DKACPIPlatform *self = user;

	if (uacpi_device_matches_pnp_id(node, pci_list))
		[self makePCIBusFromNode:node];
	else if (uacpi_device_matches_pnp_id(node,
		     (const char *const[]) { "PNP0303", "PNP030B", "PNP0320",
			 NULL }))
		[PS2Keyboard probeWithProvider:self acpiNode:node];

	return UACPI_NS_ITERATION_DECISION_CONTINUE;
}

static void
madt_walk(acpi_madt_t *madt,
    void (*callback)(acpi_madt_entry_header_t *item, void *arg), void *arg)
{
	for (uint8_t *item = &madt->entries[0]; item <
	     (madt->entries + (madt->header.length - sizeof(acpi_madt_t)));) {
		acpi_madt_entry_header_t *header = (acpi_madt_entry_header_t *)
		    item;
		callback(header, arg);
		item += header->length;
	}
}

#ifdef __amd64
static void
parse_ioapics(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_ioapic_t *ioapic;

	if (item->type != 1)
		return;

	ioapic = (acpi_madt_ioapic_t *)item;
	[[IOApic alloc] initWithProvider:arg
				      id:ioapic->ioapic_id
				 address:(paddr_t)ioapic->ioapic_addr
				 gsiBase:ioapic->gsi_base];
}

static void
parse_isa_overrides(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_int_override_t *intr;
	struct isa_intr_override *override;

	if (item->type != 2)
		return;

	intr = (acpi_madt_int_override_t *)item;
	override = &isa_intr_overrides[intr->irq_source];

	override->gsi = intr->gsi;
	override->lopol = (intr->flags & 0x2) == 0x2 ? 0x1 : 0x0;
	override->edge = (intr->flags & 0x8) == 0x8 ? 0x1 : 0x0;
}
#elif defined(__aarch64__)

static void
parse_giccs(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_gicc_t *gicc;

	if (item->type != 0xb)
		return;

	gicc = (void *)item;
	kprintf("Found a GICC: "
		"GIC interface num %d, ACPI UID %d base address 0x%zx\n",
	    gicc->cpu_interface_number, gicc->acpi_process_uid,
	    gicc->physical_base_addr);
}

static void
parse_gicds(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_gicd_t *gicd;

	if (item->type != 0xc)
		return;

	gicd = (void *)item;
	kprintf("Found a GICD: GIC version num %d, base address 0x%zx\n",
	    gicd->gic_version, gicd->physical_base_address);
}

static void
gtdt_walk(void)
{
	uacpi_table table;
	acpi_table_gtdt_t *gtdt;
	int r;

	r = uacpi_table_find_by_signature("GTDT", &table);
	if (r != 0)
		kfatal("No GTDT table found!\n");

	gtdt = (void *)table.virt_addr;

	kprintf("GTDT: %p\n", gtdt);
}
#endif


- (instancetype)initWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	struct uacpi_init_params params = {
		.rsdp = V2P(rsdp),
		.log_level = UACPI_LOG_INFO,
	};
	int r;

	self = [super initWithProvider:provider];
	kmem_asprintf(obj_name_ptr(self), "acpi-platform");
	acpiplatform = self;
	[self registerDevice];
	DKLogAttach(self);

	r = uacpi_initialize(&params);
	kassert(r == UACPI_STATUS_OK);

	uacpi_table madt;
	r = uacpi_table_find_by_signature("APIC", &madt);
	kassert(r == UACPI_STATUS_OK);

#if defined(__amd64__)
	for (int i = 0; i < 16; i++) {
		isa_intr_overrides[i].gsi = i;
		isa_intr_overrides[i].lopol = i;
		isa_intr_overrides[i].gsi = i;
	}

	madt_walk((acpi_madt_t *)madt.virt_addr, parse_ioapics, self);
	madt_walk((acpi_madt_t *)madt.virt_addr, parse_isa_overrides,
	    self);
#elif defined(__aarch64__)
	madt_walk((acpi_madt_t *)madt.virt_addr, parse_giccs, NULL);
	madt_walk((acpi_madt_t *)madt.virt_addr, parse_gicds, NULL);
	gtdt_walk();
#endif

	return self;
}

- (void)secondStageInit
{
	uacpi_namespace_node *sb;
	int r;

	DKDevLog(self, "Carrying out second-stage initialisation");

	r = uacpi_namespace_load();
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_namespace_initialize();
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
	kassert(r == UACPI_STATUS_OK);

	sb = uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB);
	kassert(sb != NULL);

	uacpi_namespace_for_each_node_depth_first(sb, iteration_callback, self);
}

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	if (rsdp->Revision > 0)
		xsdt = (acpi_xsdt_t *)P2V(((rsdp_desc2_t *)rsdp)->XsdtAddress);
	else
		rsdt = (acpi_rsdt_t *)P2V((uintptr_t)rsdp->RsdtAddress);

	[[self alloc] initWithProvider:provider rsdp:rsdp];

	return YES;
}

+ (instancetype)instance
{
	return acpiplatform;
}

@end
