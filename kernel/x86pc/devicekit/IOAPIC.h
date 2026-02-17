/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file IOAPIC.h
 * @brief I/O APIC
 */

#ifndef ECX_DEVICEKIT_IOAPIC_H
#define ECX_DEVICEKIT_IOAPIC_H

#include <keyronex/intr.h>
#include <keyronex/vm_types.h>

#include <devicekit/DKDevice.h>

struct isa_intr_override {
	uint8_t gsi;
	uint8_t lopol;
	uint8_t edge;
};

@interface IOApic : DKDevice {
	uint32_t m_id;
	vaddr_t m_vaddr;
	uint32_t m_gsi_base;
	uint32_t m_n_redirs;
	uint8_t *m_redirs; /**< map APIC PIN to IDT vector */

	TAILQ_TYPE_ENTRY(IOApic) m_ioapics_entries;
}

/**
 * Install a handler for the given GSI (i.e. IRQ.)
 *
 * The I/O APIC handling that GSI is identified and routes the interrupt; the
 * handler is then installed into the IDT by the kernel.
 *
 * @returns 0 if handler installed successfully
 */
+ (int)handleSource:(struct kirq_source *)source
	withHandler:(kirq_handler_t *)handler
	   argument:(void *)arg
	  irqObject:(kirq_t *)object
	 atPriority:(ipl_t *)ipl;

- initWithId:(uint32_t)id address:(paddr_t)paddr gsiBase:(uint32_t)gsiBase;

@end

extern struct isa_intr_override isa_intr_overrides[16];

#endif /* ECX_DEVICEKIT_IOAPIC_H */
