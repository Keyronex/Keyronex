/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sat Sep 16 2023.
 */

#ifndef KRX_AMD64_IOAPIC_H
#define KRX_AMD64_IOAPIC_H

#include <ddk/DKDevice.h>
#include <ddk/DKInterrupt.h>
#include <kdk/amd64.h>

struct isa_intr_override {
	uint8_t gsi;
	uint8_t lopol;
	uint8_t edge;
};

@interface IOApic : DKPlatformInterruptController {
	uint32_t _id;
	vaddr_t _vaddr;
	uint32_t _gsi_base;
	uint32_t _n_redirs;
	uint8_t *redirs; /**< map APIC PIN to IDT vector */

	TAILQ_TYPE_ENTRY(IOApic) _ioapics_entries;
}

/**
 * Install a handler for the given GSI (i.e. IRQ.)
 *
 * The I/O APIC handling that GSI is identified and routes the interrupt; the
 * handler is then installed into the IDT by the kernel.
 *
 * @returns 0 if handler installed successfully
 */
+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry;

- initWithId:(uint32_t)id address:(paddr_t)paddr gsiBase:(uint32_t)gsiBase;

@end

extern struct isa_intr_override isa_intr_overrides[16];

#endif /* KRX_AMD64_IOAPIC_H */
