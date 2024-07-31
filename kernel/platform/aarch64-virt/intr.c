#include <stdint.h>

#include "gic.h"
#include "kdk/aarch64.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "kern/ki.h"

static TAILQ_HEAD(intr_entries, intr_entry) intr_entries[1024];

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
plat_irq(md_intr_frame_t *frame)
{
	uint32_t intr = gengic_acknowledge();
	ipl_t ipl;

	switch (intr & 0x3ff) {
	case 2: {
		ipl = spldpc();
		ki_set_interrupts(true);
		void ki_tlb_flush_handler(void);
		ki_tlb_flush_handler();
		break;
	}

	case 4: {
		ipl = spldpc();
		ki_set_interrupts(true);
		gengic_eoi(intr);
		ki_dispatch_dpcs(curcpu());
		splx(ipl);
		ki_disable_interrupts();
		return;
		break;
	}

	case 30: {
		ipl = splraise(kIPLHigh);
		void reset_timer(void);
		reset_timer();
		ki_cpu_hardclock(frame, curcpu());
		break;
	}

	case 1023: {
		ipl = splget();
		break;
	}

	default: {
		struct intr_entries *entries = &intr_entries[intr & 0x3ff];
		struct intr_entry *entry;
		ipl = 0;

		if (TAILQ_EMPTY(entries)) {
			kfatal("Unhandled interrupt %u.", intr);
		}

		TAILQ_FOREACH (entry, entries, queue_entry) {
			ipl = MAX2(ipl, entry->ipl);
		}

		if (ipl < splget()) {
			kprintf(
			    "In handling interrupt %u:\n"
			    "IPL not less or equal (running at %d, interrupt priority %d)\n",
			    intr, splget(), ipl);
		}

		ipl = splraise(ipl);
		ki_set_interrupts(true);

		TAILQ_FOREACH (entry, entries, queue_entry) {
			bool r = entry->handler(frame, entry->arg);
			(void)r;
		}

		break;
	}
	}
	gengic_eoi(intr);
	splx(ipl);
	ki_disable_interrupts();
}

void
irq_init(void)
{
	for (int i = 0; i < elementsof(intr_entries); i++) {
		TAILQ_INIT(&intr_entries[i]);
	}
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
	TAILQ_INSERT_TAIL(&intr_entries[gsi], entry, queue_entry);
}
