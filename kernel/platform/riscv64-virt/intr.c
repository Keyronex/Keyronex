/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Aug 05 2024.
 */

#include <stdint.h>

#include "kdk/kern.h"
#include "kdk/vm.h"
#include "kern/ki.h"

void
md_raise_dpc_interrupt(void)
{
	curcpu()->cpucb.dpc_int = true;
}

ipl_t
splraise(ipl_t ipl)
{
	bool x = ki_disable_interrupts();
	ipl_t old = curcpu()->cpucb.ipl;
	kassert(ipl >= old);
	curcpu()->cpucb.ipl = ipl;
	if (ipl < kIPLHigh)
		ki_set_interrupts(x);
	return old;
}

void
splx(ipl_t to)
{
	bool x = ki_disable_interrupts();
	kcpu_t *cpu = curcpu();
	ipl_t old = cpu->cpucb.ipl;
	kassert(to <= old);

	if (old >= kIPLDPC && to < kIPLDPC) {
		cpu->cpucb.ipl = kIPLDPC;
		while (cpu->cpucb.dpc_int) {
			curcpu()->cpucb.dpc_int = 0;
			ki_set_interrupts(x);
			ki_dispatch_dpcs(cpu);
			x = ki_disable_interrupts();
		}
		cpu = curcpu(); /* could have migrated! */
	}

	cpu->cpucb.ipl = to;

	if (to < kIPLHigh)
		ki_set_interrupts(x);
}

ipl_t
splget(void)
{
	bool x = ki_disable_interrupts();
	ipl_t old = curcpu()->cpucb.ipl;
	ki_set_interrupts(x);
	return old;
}

void
md_intr_register(const char *name, uint32_t gsi, ipl_t prio,
    intr_handler_t handler, void *arg, bool shareable, struct intr_entry *entry)
{
	entry->name = name;
	entry->ipl = prio;
	entry->handler = handler;
	entry->arg = arg;
	entry->shareable = shareable;
	// TAILQ_INSERT_TAIL(&intr_entries[gsi], entry, queue_entry);
}
