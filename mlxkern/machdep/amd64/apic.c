/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */

#include "amd64.h"
#include "kernel/ke.h"
#include "vm/vm.h"

enum {
	kLAPICRegEOI = 0xb0,
	kLAPICRegSpurious = 0xf0,
	kLAPICRegICR0 = 0x300,
	kLAPICRegICR1 = 0x310,
	kLAPICRegTimer = 0x320,
	kLAPICRegTimerInitial = 0x380,
	kLAPICRegTimerCurrentCount = 0x390,
	kLAPICRegTimerDivider = 0x3e0,
};

enum {
	kLAPICTimerPeriodic = 0x20000,
};

static uint32_t
lapic_read(kcpu_t *cpu, uint32_t reg)
{
	return *(uint32_t *)P2V((cpu->hl.lapic_base & 0xfffff000) + reg);
}

static void
lapic_write(kcpu_t *cpu, uint32_t reg, uint32_t val)
{
	uint32_t *addr = P2V((cpu->hl.lapic_base & 0xfffff000) + reg);
	*addr = val;
}

void
lapic_eoi()
{
	lapic_write(hl_curcpu(), kLAPICRegEOI, 0x0);
}

void
lapic_enable(uint8_t spurvec)
{
	lapic_write(hl_curcpu(), kLAPICRegSpurious,
	    lapic_read(hl_curcpu(), kLAPICRegSpurious) | (1 << 8) | spurvec);
}

/* setup PIC to run oneshot for 1/hz sec */
static void
pit_init_oneshot(uint32_t hz)
{
	int divisor = 1193180 / hz;

	outb(0x43, 0x30 /* lohi */);

	outb(0x40, divisor & 0xFF);
	outb(0x40, divisor >> 8);
}

/* await on completion of a oneshot */
static void
pit_await_oneshot(void)
{
	do {
		/* bits 7, 6 must = 1, 5 = don't latch count, 1 = channel 0 */
		outb(0x43, (1 << 7) | (1 << 6) | (1 << 5) | (1 << 1));
	} while (!(inb(0x40) & (1 << 7))); /* check if set */
}

/* return the number of ticks per second for the lapic timer */
uint32_t
lapic_timer_calibrate(void)
{
	const uint32_t initial = 0xffffffff;
	const uint32_t hz = 50;
	uint32_t apic_after;
	static kspinlock_t calib = KSPINLOCK_INITIALISER;

	ipl_t ipl = ke_spinlock_acquire(&calib);

	lapic_write(hl_curcpu(), kLAPICRegTimerDivider, 0x2); /* divide by 8*/
	lapic_write(hl_curcpu(), kLAPICRegTimer, kIntNumLAPICTimer);

	pit_init_oneshot(hz);
	lapic_write(hl_curcpu(), kLAPICRegTimerInitial, initial);

	pit_await_oneshot();
	apic_after = lapic_read(hl_curcpu(), kLAPICRegTimerCurrentCount);
#if 0
	lapic_write(kLAPICRegTimer, 0x10000); /* disable*/
#endif

	ke_spinlock_release(&calib, ipl);

	return (initial - apic_after) * hz;
}

void
hl_clock_start()
{
	lapic_write(hl_curcpu(), kLAPICRegTimer,
	    kLAPICTimerPeriodic | kIntNumLAPICTimer);
	lapic_write(hl_curcpu(), kLAPICRegTimerInitial,
	    hl_curcpu()->hl.lapic_tps / KERN_HZ);
}

static void
send_ipi(uint32_t lapic_id, uint8_t intr)
{
	lapic_write(hl_curcpu(), kLAPICRegICR1, lapic_id << 24);
	lapic_write(hl_curcpu(), kLAPICRegICR0, intr);
}

void
hl_ipi_invlpg(kcpu_t *cpu)
{
	send_ipi(cpu->hl.lapic_id, kIntNumIPIInvlPG);
}

void
hl_ipi_reschedule(kcpu_t *cpu)
{
	send_ipi(cpu->hl.lapic_id, kIntNumRescheduleIPI);
}
