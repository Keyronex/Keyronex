#include <limine.h>
#include <string.h>

#include "dev/PCIBus.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/acpi/tables.h"
#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/object.h"
#include "uacpi/internal/namespace.h"
#include "uacpi/kernel_api.h"
#include "uacpi/namespace.h"
#include "uacpi/status.h"
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

void *
laihost_scan(const char *sig, size_t index)
{
	acpi_header_t *header;
	size_t cur = 0;

	if (memcmp(sig, "DSDT", 4) == 0) {
		acpi_fadt_t *fadt = laihost_scan("FACP", 0);
		kassert(fadt != NULL);
		return (void *)(xsdt == NULL ? P2V((uintptr_t)fadt->dsdt) :
					       P2V(fadt->x_dsdt));
	}

	if (xsdt) {
		size_t ntables = (xsdt->header.length - sizeof(acpi_header_t)) /
		    sizeof(uint64_t);
		for (size_t i = 0; i < ntables; i++) {
			header = (acpi_header_t *)P2V(xsdt->tables[i]);

			if (memcmp(header->signature, sig, 4) != 0)
				continue;

			if (cur++ == index)
				return header;
		}
	} else {
		size_t ntables = (rsdt->header.length - sizeof(acpi_header_t)) /
		    sizeof(uint32_t);
		for (size_t i = 0; i < ntables; i++) {
			header = (acpi_header_t *)P2V(
			    (uintptr_t)rsdt->tables[i]);

			if (memcmp(header->signature, sig, 4) != 0)
				continue;

			if (cur++ == index)
				return header;
		}
	}

	return NULL;
}

@implementation DKACPIPlatform

- (void)makePCIBusFromNode:(uacpi_namespace_node *)node
{
	uint64_t seg = 0, bus = 0;
	PCIBus *pcibus;
	int r;
	uacpi_object *obj;

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

	size_t depth = 0;
	// for (uacpi_namespace_node *parent = node; parent != NULL; parent =
	// parent->parent) 	kputc(' ', NULL); kprintf(" - %s\n",
	// node->name.text);

	if (uacpi_device_matches_pnp_id(node, pci_list))
		[self makePCIBusFromNode:node];

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
#elif defined(__aarch64__)
static void
parse_giccs(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_gicc_t *gicc;

	if (item->type != 0xb)
		return;

	gicc = (void *)item;
	kprintf("Found a GICC: GIC interface num %d, ACPI UID %d\n",
	    gicc->cpu_interface_number, gicc->acpi_process_uid);
}
#endif

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	if (rsdp->Revision > 0)
		xsdt = (acpi_xsdt_t *)P2V(((rsdp_desc2_t *)rsdp)->XsdtAddress);
	else
		rsdt = (acpi_rsdt_t *)P2V((uintptr_t)rsdp->RsdtAddress);

	[[self alloc] initWithProvider:provider rsdp:rsdp];

	return YES;
}

- (instancetype)initWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	struct uacpi_init_params params = {
		.rsdp = V2P(rsdp),
		.rt_params = {
			.log_level = UACPI_LOG_TRACE,
			.flags = 0,
		},
		.no_acpi_mode = 0,
	};
	uacpi_namespace_node *sb;
	int r;

	self = [super initWithProvider:provider];
	kmem_asprintf(obj_name_ptr(self), "acpi-platform");
	acpiplatform = self;
	[self registerDevice];
	DKLogAttach(self);

	acpi_madt_t *madt;
	madt = laihost_scan("APIC", 0);
	kassert(madt != NULL);
#if defined(__amd64__)
	madt_walk(madt, parse_ioapics, self);
#elif defined(__aarch64__)
	madt_walk(madt, parse_giccs, NULL);
#endif
	r = uacpi_initialize(&params);
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_namespace_load();
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_namespace_initialize();
	kassert(r == UACPI_STATUS_OK);
	r = uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_IOAPIC);
	kassert(r == UACPI_STATUS_OK);

	sb = uacpi_namespace_node_find(uacpi_namespace_root(), "_SB_");
	kassert(sb != NULL);

	uacpi_namespace_for_each_node_depth_first(sb, iteration_callback, self);

	return self;
}

@end

#if 0

#include "DKAACPIPlatform.h"
#include "PCIBus.h"
#include "ddk/DKDevice.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "lai/core.h"
#include "lai/helpers/sci.h"

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

void
laihost_log(int level, const char *msg)
{
	DKLog("acpi-platform", "lai: %s\n", msg);
}

__attribute__((noreturn)) void
laihost_panic(const char *msg)
{
	kfatal("acpi-platform: lai: %s\n", msg);
}

void *
laihost_malloc(size_t size)
{
	return kmem_alloc(size);
}

void *
laihost_realloc(void *ptr, size_t size, size_t oldsize)
{
	return kmem_realloc(ptr, oldsize, size);
}

void
laihost_free(void *ptr, size_t size)
{
	return kmem_free(ptr, size);
}

void *
laihost_map(size_t address, size_t count)
{
	return (void *)P2V(address);
}

void
laihost_unmap(void *pointer, size_t count)
{
	/* nop */
}

#ifdef AMD64
void
laihost_outb(uint16_t port, uint8_t val)
{
	asm volatile("outb %0, %1" : : "a"(val), "d"(port));
}
void
laihost_outw(uint16_t port, uint16_t val)
{
	asm volatile("outw %0, %1" : : "a"(val), "d"(port));
}
void
laihost_outd(uint16_t port, uint32_t val)
{
	asm volatile("outl %0, %1" : : "a"(val), "d"(port));
}

uint8_t
laihost_inb(uint16_t port)
{
	uint8_t val;
	asm volatile("inb %1, %0" : "=a"(val) : "d"(port));
	return val;
}
uint16_t
laihost_inw(uint16_t port)
{
	uint16_t val;
	asm volatile("inw %1, %0" : "=a"(val) : "d"(port));
	return val;
}
uint32_t
laihost_ind(uint16_t port)
{
	uint32_t val;
	asm volatile("inl %1, %0" : "=a"(val) : "d"(port));
	return val;
}

uint8_t
pci_readb(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inb(0xCFC + (offset & 3));
}

uint16_t
pci_readw(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inw(0xCFC + (offset & 3));
}

uint32_t
pci_readl(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	return inl(0xCFC + (offset & 3));
}

void
pci_writeb(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset,
    uint8_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outb(0xCFC + (offset & 3), value);
}

void
pci_writew(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset,
    uint16_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outw(0xCFC + (offset & 3), value);
}

void
pci_writel(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset,
    uint32_t value)
{
	uint32_t address = (bus << 16) | (slot << 11) | (function << 8) |
	    (offset & ~(uint32_t)(3)) | 0x80000000;
	outl(0xCF8, address);
	outl(0xCFC + (offset & 3), value);
}
#elif 0
uint8_t
pci_readb(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
}

uint16_t
pci_readw(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
}

uint32_t
pci_readl(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset)
{
}

void
pci_writeb(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset,
    uint8_t value)
{
}

void
pci_writew(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset,
    uint16_t value)
{
}

void
pci_writel(uint32_t bus, uint32_t slot, uint32_t function, uint32_t offset,
    uint32_t value)
{
}
#endif

#ifdef AMD64
void
laihost_pci_writeb(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset, uint8_t v)
{
	kassert(seg == 0);
	return pci_writeb(bus, slot, fn, offset, v);
}
void
laihost_pci_writew(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset, uint16_t v)
{
	kassert(seg == 0);
	return pci_writew(bus, slot, fn, offset, v);
}
void
laihost_pci_writed(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset, uint32_t v)
{
	kassert(seg == 0);
	return pci_writel(bus, slot, fn, offset, v);
}

uint8_t
laihost_pci_readb(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset)
{
	kassert(seg == 0);
	return pci_readb(bus, slot, fn, offset);
}
uint16_t
laihost_pci_readw(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset)
{
	kassert(seg == 0);
	return pci_readw(bus, slot, fn, offset);
}
uint32_t
laihost_pci_readd(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset)
{
	kassert(seg == 0);
	return pci_readl(bus, slot, fn, offset);
}
#endif

void
laihost_sleep(uint64_t ms)
{
	for (size_t i = 0; i < 1000 * ms; i++) {
		// asm("pause");
	}
}

char
i2hex(uint64_t val, uint32_t pos)
{
	int digit = (val >> pos) & 0xf;
	if (digit < 0xa)
		return '0' + digit;
	else
		return 'A' + (digit - 10);
}

int
hex2i(char character)
{
	if (character <= '9')
		return character - '0';
	else if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	kassert(!"unreached.");
}

static const char *
eisaid_to_string(uint32_t num)
{
	static char out[8];

	num = __builtin_bswap32(num);

	out[0] = (char)(0x40 + ((num >> 26) & 0x1F));
	out[1] = (char)(0x40 + ((num >> 21) & 0x1F));
	out[2] = (char)(0x40 + ((num >> 16) & 0x1F));
	out[3] = i2hex((uint64_t)num, 12);
	out[4] = i2hex((uint64_t)num, 8);
	out[5] = i2hex((uint64_t)num, 4);
	out[6] = i2hex((uint64_t)num, 0);
	out[7] = 0;

	return out;
}

uint32_t
string_to_eisaid(const char *id)
{
	uint32_t out = 0;
	out |= ((id[0] - 0x40) << 26);
	out |= ((id[1] - 0x40) << 21);
	out |= ((id[2] - 0x40) << 16);
	out |= hex2i(id[3]) << 12;
	out |= hex2i(id[4]) << 8;
	out |= hex2i(id[5]) << 4;
	out |= hex2i(id[6]);

	return __builtin_bswap32(out);
}

static int
laiex_eval_one_int(lai_nsnode_t *node, const char *path, uint64_t *out,
    lai_state_t *state)
{
	LAI_CLEANUP_VAR lai_variable_t var = LAI_VAR_INITIALIZER;
	lai_nsnode_t *handle;
	int r;

	handle = lai_resolve_path(node, path);
	if (handle == NULL)
		return LAI_ERROR_NO_SUCH_NODE;

	r = lai_eval(&var, handle, state);
	if (r != LAI_ERROR_NONE)
		return r;

	return lai_obj_get_integer(&var, out);
}

@implementation DKACPIPlatform

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
#elif defined(__aarch64__)
static void
parse_giccs(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_gicc_t *gicc;

	if (item->type != 0xb)
		return;

	gicc = (void *)item;
	kprintf("Found a GICC: GIC interface num %d, ACPI UID %d\n",
	    gicc->cpu_interface_number, gicc->acpi_process_uid);
	//for (;;)
	//	;
}
#endif

+ (BOOL)probeWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	if (rsdp->Revision > 0)
		xsdt = (acpi_xsdt_t *)P2V(((rsdp_desc2_t *)rsdp)->XsdtAddress);
	else
		rsdt = (acpi_rsdt_t *)P2V((uintptr_t)rsdp->RsdtAddress);

	[[self alloc] initWithProvider:provider rsdp:rsdp];

	return YES;
}

- (void)makePCIBusFromNode:(lai_nsnode_t *)node
{
	uint64_t seg = -1, bus = -1;
	LAI_CLEANUP_STATE lai_state_t state;
	PCIBus *pcibus;
	int r;

	lai_init_state(&state);

	r = laiex_eval_one_int(node, "_SEG", &seg, &state);
	if (r != LAI_ERROR_NONE) {
		if (r == LAI_ERROR_NO_SUCH_NODE) {
			seg = 0;
		} else {
			DKLog("PCIBus", "failed to evaluate _SEG: %d\n", r);
			return;
		}
	}

	r = laiex_eval_one_int(node, "_BBN", &bus, &state);
	if (r != LAI_ERROR_NONE) {
		if (r == LAI_ERROR_NO_SUCH_NODE) {
			bus = 0;
		} else {
			DKLog("PCIBus", "failed to evaluate _BBN: %d\n", r);
			return;
		}
	}

	pcibus = [[PCIBus alloc] initWithProvider:self  segment:seg bus:bus];
	(void)pcibus;
}

- (void)matchDev:(lai_nsnode_t *)node depth:(int)depth
{
	LAI_CLEANUP_STATE lai_state_t state;
	LAI_CLEANUP_VAR lai_variable_t id = { 0 };
	lai_nsnode_t *hhid;
	const char *devid;

	lai_init_state(&state);

	hhid = lai_resolve_path(node, "_HID");
	if (hhid != NULL) {
		if (lai_eval(&id, hhid, &state))
			lai_warn("could not evaluate _HID of device");
		else
			LAI_ENSURE(id.type);
	}

	if (!id.type) {
		lai_nsnode_t *hcid = lai_resolve_path(node, "_CID");
		if (hcid != NULL) {
			if (lai_eval(&id, hcid, &state))
				lai_warn("could not evaluate _CID of device");
			else
				LAI_ENSURE(id.type);
		}
	}

	if (!id.type)
		return;

	if (id.type == LAI_STRING)
		devid = lai_exec_string_access(&id);
	else
		devid = eisaid_to_string(id.integer);

	for (int i = 0; i < depth; i++)
		pac_printf(" ");

	if (strcmp(devid, ACPI_PCI_ROOT_BUS_PNP_ID) == 0 ||
	    strcmp(devid, ACPI_PCIE_ROOT_BUS_PNP_ID) == 0) {
		[self makePCIBusFromNode:node];
	}
}

/* depth-first traversal of devices within the tree */
- (void)iterate:(lai_nsnode_t *)obj depth:(int)depth
{
	struct lai_ns_child_iterator iterator =
	    LAI_NS_CHILD_ITERATOR_INITIALIZER(obj);
	lai_nsnode_t *node;

	while ((node = lai_ns_child_iterate(&iterator))) {
		if (node->type != 6)
			continue; /* not a device */

		[self matchDev:node depth:depth];

		if (node->children.num_elems)
			[self iterate:node depth:depth + 2];
	}
}

- (instancetype)initWithProvider:(DKDevice *)provider rsdp:(rsdp_desc_t *)rsdp
{
	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "acpi-platform");
	acpiplatform = self;

#ifndef AMD64
	mcfg_t *mcfg = laihost_scan("MCFG", 0);
	kassert(mcfg != NULL);
	size_t mcfg_count = (mcfg->header.length - sizeof(mcfg_t)) /
	    sizeof(struct allocation);
	for (size_t i = 0; i < mcfg_count; i++) {
		struct allocation *alloc = &mcfg->allocations[i];
		size_t size = (alloc->EndBus - alloc->StartBus + 1) * 0x100000;
		kprintf("Seg %lu Bus %d-%d Base 0x%lx\n", alloc->Segn,
		    alloc->StartBus, alloc->EndBus, alloc->Base);
		for (size_t i = 0; i < size; i += PGSIZE) {
			/*
			 * this simply isn't reasonable, we need large pages
			 */
		}
	}
#endif

	lai_set_acpi_revision(rsdp->Revision);
	lai_create_namespace();
	lai_enable_acpi(1);

	[self registerDevice];
	DKLogAttach(self);

	acpi_madt_t *madt;
	madt = laihost_scan("APIC", 0);
	kassert(madt != NULL);
#if defined(__amd64__)
	madt_walk(madt, parse_ioapics, self);
#elif defined(__aarch64__)
	madt_walk(madt, parse_giccs, NULL);
#endif

	[self iterate:lai_resolve_path(NULL, "_SB_") depth:0];

	return self;
}

@end
#endif
