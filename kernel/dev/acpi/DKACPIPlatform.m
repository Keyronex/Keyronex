#include <limine.h>
#include <string.h>

#include "dev/PCIBus.h"
#include "dev/PS2Keyboard.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/tables.h"
#include "kdk/executive.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
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

	pcibus = [[PCIBus alloc] initWithProvider:self
					 acpiNode:node
					  segment:seg
					      bus:bus];
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
madt_walk(struct acpi_madt *madt,
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

#ifdef __amd64
static void
parse_ioapics(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_ioapic *ioapic;

	if (item->type != 1)
		return;

	ioapic = (struct acpi_madt_ioapic *)item;
	[[IOApic alloc] initWithProvider:arg
				      id:ioapic->id
				 address:(paddr_t)ioapic->address
				 gsiBase:ioapic->gsi_base];
}

static void
parse_isa_overrides(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_interrupt_source_override *intr;
	struct isa_intr_override *override;

	if (item->type != 2)
		return;

	intr = (struct acpi_madt_interrupt_source_override *)item;
	override = &isa_intr_overrides[intr->source];

	override->gsi = intr->gsi;
	override->lopol = (intr->flags & 0x2) == 0x2 ? 0x1 : 0x0;
	override->edge = (intr->flags & 0x8) == 0x8 ? 0x1 : 0x0;
}
#elif defined(__aarch64__)

static paddr_t last_gicc_base = 0;

static void
parse_giccs(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_gicc *gicc;
	bool matched = false;

	if (item->type != 0xb)
		return;

	gicc = (void *)item;
	kprintf(
	    "Found a GICC: "
	    "GIC interface num %u, ACPI UID %u, MPIDR %lu, base address 0x%zx\n",
	    gicc->interface_number, gicc->acpi_id, gicc->mpidr, gicc->address);

	if (last_gicc_base != 0)
		kassert(last_gicc_base == gicc->address);

	for (size_t i = 0; i < ncpus; i++) {
		if (cpus[i]->cpucb.mpidr == gicc->mpidr) {
			int r;

			kassert(!matched);
			matched = true;

			cpus[i]->cpucb.gic_interface_number =
			    gicc->interface_number;

			r = vm_ps_map_physical_view(kernel_process->vm,
			    &cpus[i]->cpucb.gicc_base, PGSIZE, gicc->address,
			    kVMAll, kVMAll, false);
			kassert(r == 0);
		}
	}

	kassert(matched);
}

static void
parse_gicds(struct acpi_entry_hdr *item, void *arg)
{
	struct acpi_madt_gicd *gicd;
	extern vaddr_t gicd_base;
	int r;

	if (item->type != 0xc)
		return;

	gicd = (void *)item;
	kprintf("Found a GICD: GIC version num %d, base address 0x%zx\n",
	    gicd->gic_version, gicd->address);

	r = vm_ps_map_physical_view(kernel_process->vm, &gicd_base, PGSIZE,
	    gicd->address, kVMAll, kVMAll, false);
	kassert(r == 0);
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

	kprintf("GTDT: %p/ EL1 NS: GSIV %u, Flags 0x%x\n", gtdt,
	    gtdt->nonsecure_el1_interrupt, gtdt->nonsecure_el1_flags);
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
		[PCIBus setECAMBase:entry->address];
	}
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

	madt_walk((struct acpi_madt *)madt.virt_addr, parse_ioapics, self);
	madt_walk((struct acpi_madt *)madt.virt_addr, parse_isa_overrides,
	    self);
#elif defined(__aarch64__)
	madt_walk((struct acpi_madt *)madt.virt_addr, parse_giccs, NULL);
	madt_walk((struct acpi_madt *)madt.virt_addr, parse_gicds, NULL);
	gtdt_walk();
	mcfg_walk();
#endif

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

	uacpi_namespace_for_each_node_depth_first(sb, iteration_callback, self);
}

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	[[self alloc] initWithProvider:provider rsdp:rsdp];

	return YES;
}

+ (instancetype)instance
{
	return acpiplatform;
}

@end
