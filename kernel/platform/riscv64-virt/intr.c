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
}

void
splx(ipl_t to)
{
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
