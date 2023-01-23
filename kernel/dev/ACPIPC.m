#include <byteswap.h>
#include <amd64/amd64.h>

#include <dev/ACPIPC.h>
#include <dev/PCIBus.h>
#include <dev/IOApic.h>
#include <kern/kmem.h>
#include <libkern/libkern.h>

#include "devicekit/DKDevice.h"
#include "lai/core.h"
#include "lai/helpers/sci.h"
#include "vm/vm.h"

typedef struct {
	acpi_header_t header;
	uint32_t      lapic_addr;
	uint32_t      flags;
	uint8_t	      entries[0];
} __attribute__((packed)) acpi_madt_t;

typedef struct {
	uint8_t type;
	uint8_t length;
} __attribute__((packed)) acpi_madt_entry_header_t;

/* MADT entry type 1 */
typedef struct {
	acpi_madt_entry_header_t header;
	uint8_t			 ioapic_id;
	uint8_t			 reserved;
	uint32_t		 ioapic_addr;
	uint32_t		 gsi_base;
} __attribute__((packed)) acpi_madt_ioapic_t;

/* MADT entry type 2 */
typedef struct {
	acpi_madt_entry_header_t header;
	uint8_t			 bus_source;
	uint8_t			 irq_source;
	uint32_t		 gsi;
	uint16_t		 flags;
} __attribute__((packed)) acpi_madt_int_override_t;

acpi_rsdt_t *rsdt = NULL;
acpi_xsdt_t *xsdt = NULL;
mcfg_t      *mcfg = NULL;
static AcpiPC *acpipc = NULL;

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

void
laihost_log(int level, const char *msg)
{
	DKLog("AcpiPC", "lai: %s\n", msg);
}

__attribute__((noreturn)) void
laihost_panic(const char *msg)
{
	fatal("AcpiPC: lai: %s\n", msg);
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

void *
laihost_scan(const char *sig, size_t index)
{
	acpi_header_t *header;
	size_t	       cur = 0;

	if (memcmp(sig, "DSDT", 4) == 0) {
		acpi_fadt_t *fadt = laihost_scan("FACP", 0);
		assert(fadt != NULL);
		return xsdt == NULL ? P2V((uintptr_t)fadt->dsdt) :
				      P2V(fadt->x_dsdt);
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

void
laihost_pci_writeb(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset, uint8_t v)
{
	assert(seg == 0);
	return pci_writeb(bus, slot, fn, offset, v);
}
void
laihost_pci_writew(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset, uint16_t v)
{
	assert(seg == 0);
	return pci_writew(bus, slot, fn, offset, v);
}
void
laihost_pci_writed(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset, uint32_t v)
{
	assert(seg == 0);
	return pci_writel(bus, slot, fn, offset, v);
}

uint8_t
laihost_pci_readb(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset)
{
	assert(seg == 0);
	return pci_readb(bus, slot, fn, offset);
}
uint16_t
laihost_pci_readw(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset)
{
	assert(seg == 0);
	return pci_readw(bus, slot, fn, offset);
}
uint32_t
laihost_pci_readd(uint16_t seg, uint8_t bus, uint8_t slot, uint8_t fn,
    uint16_t offset)
{
	assert(seg == 0);
	return pci_readl(bus, slot, fn, offset);
}

void
laihost_sleep(uint64_t ms)
{
	for (size_t i = 0; i < 1000 * ms; i++) {
		asm("pause");
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
	assert(!"unreached.");
}

static const char *
eisaid_to_string(uint32_t num)
{
	static char out[8];

	num = bswap_32(num);

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

	return bswap_32(out);
}

@implementation AcpiPC : DKDevice

- (void)matchDev:(lai_nsnode_t *)node
{
	LAI_CLEANUP_STATE lai_state_t  state;
	LAI_CLEANUP_VAR lai_variable_t id = { 0 };
	lai_nsnode_t		     *hhid;
	const char		    *devid;

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

	/* TODO: matching on device classes by a more elegant means */
	/*if (strcmp(devid, "PNP0303") == 0)
		[PS2Keyboard probeWithAcpiNode:node];
	else */
	if (strcmp(devid, ACPI_PCI_ROOT_BUS_PNP_ID) == 0) {
		[PCIBus probeWithAcpiNode:node provider:self];
	} else if (strcmp(devid, ACPI_PCIE_ROOT_BUS_PNP_ID) == 0) {
		[PCIBus probeWithAcpiNode:node provider:self];
	}
}

/* depth-first traversal of devices within the tree */
- (void)iterate:(lai_nsnode_t *)obj
{
	struct lai_ns_child_iterator iterator =
	    LAI_NS_CHILD_ITERATOR_INITIALIZER(obj);
	lai_nsnode_t *node;

	while ((node = lai_ns_child_iterate(&iterator))) {
		if (node->type != 6)
			continue; /* not a device */

		[self matchDev:node];

		if (node->children.num_elems)
			[self iterate:node];
	}
}

+ instance
{
	return acpipc;
}

+ (BOOL)probeWithRSDP:(rsdp_desc_t *)rsdp
{
	if (rsdp->Revision > 0)
		xsdt = (acpi_xsdt_t *)P2V(((rsdp_desc2_t *)rsdp)->XsdtAddress);
	else
		rsdt = (acpi_rsdt_t *)P2V((uintptr_t)rsdp->RsdtAddress);

	lai_set_acpi_revision(rsdp->Revision);
	lai_create_namespace();
	lai_enable_acpi(1);

	[[self alloc] init];

	return YES;
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

static void
parse_ioapics(acpi_madt_entry_header_t *item, void *arg)
{
	acpi_madt_ioapic_t *ioapic;

	if (item->type != 1)
		return;

	ioapic = (acpi_madt_ioapic_t *)item;
	[[IOApic alloc] initWithID:ioapic->ioapic_id
			   address:(void *)(uintptr_t)ioapic->ioapic_addr
			   gsiBase:ioapic->gsi_base];
}

static void
parse_intrs(acpi_madt_entry_header_t *item, void *arg)
{
	switch (item->type) {
	case 2: {
		acpi_madt_int_override_t *intr = (acpi_madt_int_override_t *)
		    item;
		DKLog("AcpiPC0",
		    "Remap ISA IRQ %d to %d (lopol %d, "
		    "lvltrig %d)\n",
		    intr->irq_source, intr->gsi,
		    (intr->flags & 0x2) == 0x2 ? 0x1 : 0x0,
		    (intr->flags & 0x8) == 0x8 ? 0x1 : 0x0);
		break;
	}
	}
}

#if 0
static void
laiex_create_integer(lai_variable_t *var, uint64_t val)
{
	var->type = LAI_TYPE_INTEGER;
	var->integer = val;
}

static void
do_osc(lai_nsnode_t *osc)
{
	const char *strUuid = "0811b06e-4a27-44f9-8d60-3cbbc22e7b48";
	LAI_CLEANUP_STATE lai_state_t  state;
	LAI_CLEANUP_VAR lai_variable_t uuid, revId, cntCap, caps, ret;
	uint32_t		      *u32Ret;
	int			       r;

	lai_init_state(&state);

	r = lai_create_buffer(&uuid, strlen(strUuid));
	memcpy(lai_exec_buffer_access(&uuid), strUuid, strlen(strUuid));
	laiex_create_integer(&revId, 1);
	laiex_create_integer(&cntCap, 2);
	r = lai_create_buffer(&caps, 2);
	memset(lai_exec_buffer_access(&caps), 0x0, 2);

	r = lai_eval_largs(&ret, osc, &state, uuid, revId, cntCap, caps, NULL);
	if (r != LAI_ERROR_NONE) {
		DKLog("AcpiPC: failed to evaluate _OSC (error %d)\n", r);
		return;
	}

	if (lai_exec_buffer_size(&ret) != lai_exec_buffer_size(&caps)) {
		kprintf("AcpiPC: _OSC return value of unexpected length");
		return;
	}

	u32Ret = (uint32_t *)lai_exec_buffer_access(&ret);
	if (u32Ret[0] & 0x2) {
		kprintf("AcpiPC: _OSC failure\n");
	}
	if (u32Ret[0] & 0x4) {
		kprintf("AcpiPC: _OSC failed to recognise UUID\n");
	}
	if (u32Ret[0] & 0x8) {
		kprintf("AcpiPC: _OSC failed to recognise revision\n");
	}
}
#endif

- init
{
	acpi_madt_t *madt;

	self = [super init];

	kmem_asprintf(&m_name, "AcpiPC0");
	[self registerDevice];
	DKLogAttach(self);
	acpipc = self;

	madt = laihost_scan("APIC", 0);
	madt_walk(madt, parse_ioapics, NULL);
	madt_walk(madt, parse_intrs, NULL);

	mcfg = laihost_scan("MCFG", 0);

	[self iterate:lai_resolve_path(NULL, "_SB_")];

	return self;
}

@end
