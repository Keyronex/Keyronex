/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Feb 22 2023.
 */

#ifndef KRX_ACPIPC_IOAPIC_HH
#define KRX_ACPIPC_IOAPIC_HH

#include "../mdf/mdfdev.hh"
#include "acpipc.hh"
#include "bsdqueue/list.hh"

class IOApic : public Device {
	list_node<IOApic> ioapic_link;
	uint32_t id;
	vaddr_t virt_base;
	uint32_t gsi_base;
	size_t n_redirections;
	/*! pin to IDT vector */
	uint8_t redirections[24];

	static list<IOApic, &IOApic::ioapic_link> ioapic_list;

    public:
	IOApic(AcpiPC *provider, uint32_t id, paddr_t address,
	    uint32_t gsi_base);

	/*!
	 * Install a handler for the given GSI (i.e. IRQ.)
	 *
	 * The I/O APIC handling that GSI is identified and routes the
	 * interrupt; the handler is then installed into the IDT by the kernel.
	 *
	 * \p isEdgeTriggered whether the interrupt is edge-triggered.
	 * Edge-triggered interrupts cannot share a vector.
	 *
	 * @returns 0 if handler installed successfully
	 */
	static int handleGSI(uint32_t gsi, intr_handler_t handler, void *arg,
	    bool lopol, bool edge, ipl_t ipl, struct intr_entry *entry, bool shareable = true);
};

#endif /* KRX_ACPIPC_IOAPIC_HH */
