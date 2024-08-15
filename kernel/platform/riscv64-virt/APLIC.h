/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Thu Aug 08 2024.
 */

#ifndef KRX_RISCV64_APLIC_H
#define KRX_RISCV64_APLIC_H

#include "ddk/DKDevice.h"

@class DKACPIPlatform;
struct acpi_madt_aplic;
struct aplic_mmio;

@interface APLIC : DKDevice {
	volatile struct aplic_mmio *m_mmio;
}

+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry;

- (instancetype)initWithProvider:(DKACPIPlatform *)provider
		  madtAplicEntry:(struct acpi_madt_aplic *)apic_entry;

@end

#endif /* KRX_RISCV64_APLIC_H */
