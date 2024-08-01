#include <stdint.h>

#include "gic.h"
#include "kdk/aarch64.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "kern/ki.h"

static TAILQ_HEAD(intr_entries, intr_entry) intr_entries[1024];

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

#if 1
	if (old >= kIPLDPC && to < kIPLDPC) {
		if (cpu->cpucb.hard_ipl > kIPLDPC) {
			gengic_setpmr(ipl_to_pmr[kIPLDPC]);
			cpu->cpucb.hard_ipl = kIPLDPC;
		}
		cpu->cpucb.ipl = kIPLDPC;
		while (cpu->cpucb.dpc_int) {
			curcpu()->cpucb.dpc_int = 0;
			ki_set_interrupts(x);
			ki_dispatch_dpcs(cpu);
			x = ki_disable_interrupts();
		}
		cpu = curcpu(); /* could have migrated! */
	}
#endif

	cpu->cpucb.ipl = to;
	if (cpu->cpucb.hard_ipl > to) {
		gengic_setpmr(ipl_to_pmr[to]);
		cpu->cpucb.hard_ipl = to;
	}

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
handle_irq(md_intr_frame_t *frame)
{
	uint32_t intr;
	ipl_t ipl;

	intr = gengic_acknowledge();

	switch (intr & 0x3ff) {
	case kGSITLBFlush: {
		void ki_tlb_flush_handler(void);

		ki_tlb_flush_handler();
		gengic_eoi(intr);
		return;
	}

	case kGSIDPC: {
		ipl = spldpc();
		ki_set_interrupts(true);
		gengic_eoi(intr);
		ki_dispatch_dpcs(curcpu());
		splx(ipl);

		ki_disable_interrupts();
		return;
	}

	case kGSIHardclock: {
		void reset_timer(void);

		ipl = splraise(kIPLHigh);
		reset_timer();
		ki_cpu_hardclock(frame, curcpu());
		gengic_eoi(intr);
		splx(ipl);
		ki_disable_interrupts();
		return;
	}

	case 1023: { /* spurious */
		gengic_eoi(intr);
		return;
	}

	default: {
		struct intr_entries *entries = &intr_entries[intr & 0x3ff];
		struct intr_entry *entry;

		if (TAILQ_EMPTY(entries)) {
			kfatal("Unhandled interrupt %u.", intr);
		}

		ipl = 0;
		TAILQ_FOREACH (entry, entries, queue_entry) {
			ipl = MAX2(ipl, entry->ipl);
		}

		if (ipl < splget()) {
			kprintf(
			    "In handling interrupt %u:\n"
			    "IPL not less or equal (running at %d, interrupt priority %d)\n",
			    intr, splget(), ipl);
		}

		kassert(ipl == kIPLHigh);

		ipl = splraise(ipl);
		ki_set_interrupts(true);

		TAILQ_FOREACH (entry, entries, queue_entry) {
			bool r = entry->handler(frame, entry->arg);
			(void)r;
		}

		gengic_eoi(intr);
		splx(ipl);
		ki_disable_interrupts();
		return;
	}
	}
}

void
plat_irq(md_intr_frame_t *frame)
{
	uint32_t hppir;
	ipl_t irq_ipl;

	hppir = gengic_hppir();
	switch (hppir & 0x3ff) {
	case kGSIDPC:
		irq_ipl = kIPLDPC;
		break;

	case kGSITLBFlush:
	case kGSIHardclock:
		irq_ipl = kIPLHigh;
		break;

	default:
		irq_ipl = kIPLHigh;
		break;
	}

	if (irq_ipl <= curcpu()->cpucb.ipl) {
		gengic_setpmr(ipl_to_pmr[curcpu()->cpucb.ipl]);
		curcpu()->cpucb.hard_ipl = curcpu()->cpucb.ipl;
		kprintf_nospl(
		    "\e[0;31m[%d] rejected excess ipl interrupt (int %d, my ipl %d)\e[0m\n",
		    curcpu()->num, hppir & 0x3ff, curcpu()->cpucb.ipl);
		return;
	}

	handle_irq(frame);
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
