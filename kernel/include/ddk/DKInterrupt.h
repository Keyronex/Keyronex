/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sun Feb 16 2025.
 */

#ifndef KRX_DDK_DKINTERRUPT_H
#define KRX_DDK_DKINTERRUPT_H

#include <kdk/kern.h>

#include <ddk/DKDevice.h>

#include <stdbool.h>
#include <stdint.h>

@class DKPlatformInterruptController;

typedef struct dk_interrupt_source {
	DKPlatformInterruptController *controller;
	uintptr_t id;
	bool low_polarity;
	bool edge;
} dk_interrupt_source_t;

@interface DKPlatformInterruptController : DKDevice

+ (int)handleSource:(dk_interrupt_source_t *)source
	withHandler:(intr_handler_t)handler
	   argument:(void *)arg
	 atPriority:(ipl_t)prio
	      entry:(struct intr_entry *)entry;

@end

#endif /* KRX_DDK_DKINTERRUPT_H */
