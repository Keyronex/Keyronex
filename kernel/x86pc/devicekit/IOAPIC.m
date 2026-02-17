/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file IOAPIC.m
 * @brief I/O APIC
 */

#include <devicekit/IOAPIC.h>
#include <keyronex/dlog.h>
#include <keyronex/kmem.h>
#include <keyronex/vm.h>

#include "keyronex/intr.h"

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

struct isa_intr_override isa_intr_overrides[16];
static TAILQ_TYPE_HEAD(, IOApic) ioapics = TAILQ_HEAD_INITIALIZER(ioapics);

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

@implementation IOApic

- initWithId:(uint32_t)id address:(paddr_t)paddr gsiBase:(uint32_t)gsiBase
{
	int r;

	self = [super init];

	r = vm_k_map_phys(&m_vaddr, paddr, PGSIZE, kCacheModeDefault);
	kassert(r == 0);

	m_id = id;
	m_gsi_base = gsiBase;
	m_n_redirs = ((ioapic_read(m_vaddr, kRegisterVersion) >> 16) & 0xff) +
	    1;
	kassert(m_n_redirs <= 240);

	m_redirs = kmem_alloc(sizeof(uint8_t) * m_n_redirs);
	for (int i = 0; i < m_n_redirs; i++)
		m_redirs[i] = 0;

	TAILQ_INSERT_TAIL(&ioapics, self, m_ioapics_entries);

	kmem_asprintf(&m_name, "ioapic-%d", id);

	return self;
}

+ (int)handleSource:(kirq_source_t *)source
	withHandler:(kirq_t)handler
	 atPriority:(ipl_t)prio
{
	IOApic *ioapic;
	uint8_t gsi = source->source;
	bool found = false;

	TAILQ_FOREACH(ioapic, &ioapics, m_ioapics_entries) {
		if (ioapic->m_gsi_base <= gsi &&
		    ioapic->m_gsi_base + ioapic->m_n_redirs > gsi) {
			uint8_t vec;
			uint8_t intr = gsi - ioapic->m_gsi_base;
			int r;

			kassert(ioapic->m_redirs[intr] == 0 && "shared");

#if 0
			r = md_intr_alloc("gsi", prio, handler, arg,
			    !source->edge, &vec, entry);
#else
			r = -1;
			vec = 0;
#endif

			if (r != 0) {
				kdprintf(
				    "ioapic: failed to register interrupt for GSI %d: "
				    "md_intr_alloc returned %d\n",
				    gsi, r);
				return -1;
			}

			ioapic_route(ioapic->m_vaddr, intr, vec,
			    source->low_polarity, source->edge);
			ioapic->m_redirs[intr] = vec;

			found = true;
			break;
		}
	}

	if (!found) {
		kdprintf("no I/O APIC found for GSI %d\n", gsi);
		return -1;
	}

	return 0;
}

@end
