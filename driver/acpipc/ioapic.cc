/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Feb 22 2023.
 */

#include "kdk/amd64/mdamd64.h"
#include "kdk/vm.h"

#include "ioapic.hh"

enum {
	kDeliveryModeFixed = 0x0,
	kDeliveryModeLowPriority = 0x1,
	kDeliveryModeSmi = 0x2,
	kDeliveryModeNmi = 0x4,
	kDeliveryModeInit = 0x5,
	kDeliveryModeExtInt = 0x7
};

enum {
	kDestinationModePhysical = 0x0,
	kDestinationModeLogical = 0x1,
};

enum {
	kRegisterId = 0x0,
	kRegisterVersion = 0x1,
	kRegisterArbitrationPriority = 0x2,
	kRegisterRedirectionEntriesBase = 0x10,
};

list<IOApic, &IOApic::ioapic_link> IOApic::ioapic_list;
static unsigned sequence_num = 0;

static inline uint32_t
redirection_register(uint32_t entry)
{
	return kRegisterRedirectionEntriesBase + entry * 2;
}

static uint32_t
ioapic_read(vaddr_t vaddr, uint32_t reg)
{
	volatile uint32_t *base = (volatile uint32_t *)vaddr;
	base[0] = reg;
	return base[4];
}

static void
ioapic_write(vaddr_t vaddr, uint32_t reg, uint32_t val)
{
	volatile uint32_t *base = (volatile uint32_t *)vaddr;
	base[0] = reg;
	base[4] = val;
}

static void
ioapic_route(vaddr_t vaddr, uint8_t i, uint8_t vec, bool lopol, bool edge)
{
	uint64_t ent = vec;
	// ent |= kDeliveryModeLowPriority << 8;
	// ent |= kDestinationModeLogical << 11;
	ent |= kDeliveryModeFixed << 8;
	ent |= kDestinationModePhysical << 11;
	ent |= 0ul << 56; /* lapic id 0 */
	if (lopol)
		ent |= 1 << 13; /* polarity low */
	if (!edge)
		ent |= 1 << 15; /* level triggered */
	ioapic_write(vaddr, redirection_register(i), ent);
	ioapic_write(vaddr, redirection_register(i) + 1, ent >> 32);
}

IOApic::IOApic(AcpiPC *provider, uint32_t id, paddr_t address,
    uint32_t gsi_base)
    : id(id)
    , virt_base((vaddr_t)P2V(address))
    , gsi_base(gsi_base)
{
	kmem_asprintf(&objhdr.name, "ioapic%u", sequence_num++);
	attach(provider);

	n_redirections = ((ioapic_read(virt_base, kRegisterVersion) >> 16) &
			     0xff) +
	    1;
	kassert(n_redirections <= 24);

	for (unsigned i = 0; i < elementsof(redirections); i++)
		redirections[i] = 0;

	ioapic_list.insert_tail(this);
}

int
IOApic::handleGSI(uint32_t gsi, intr_handler_t handler, void *arg, bool lopol,
    bool edge, ipl_t ipl, struct intr_entry *entry, bool shareable)
{
	IOApic *ioapic;
	bool found = false;

	if (edge)
		shareable = false;

	CXXLIST_FOREACH(ioapic, &ioapic_list, )
	{
		if (ioapic->gsi_base <= gsi &&
		    ioapic->gsi_base + ioapic->n_redirections > gsi) {
			uint8_t vec;
			uint8_t intr = gsi - ioapic->gsi_base;
			int r;

			kassert(ioapic->redirections[intr] == 0 && "shared");

			r = md_intr_alloc("gsi", ipl, handler, arg,
			    shareable, &vec, entry);
			if (r != 0) {
				kdprintf(
				    "ioapic: failed to register interrupt for GSI %d: "
				    "md_intr_alloc returned %d\n",
				    gsi, r);
				return -1;
			}

			ioapic_route(ioapic->virt_base, intr, vec, lopol, edge);
			ioapic->redirections[intr] = vec;

			found = true;
			break;
		}
	}

	if (!found) {
		kdprintf("ioapic: no I/O APIC found for GSI %d\n", gsi);
		return -1;
	}

	return 0;
}