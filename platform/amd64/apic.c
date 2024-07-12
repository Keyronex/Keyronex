/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */

#include "kdk/amd64/portio.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "intr.h"
#include "kern/ki.h"

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
	return *(volatile uint32_t *)P2V(
	    (cpu->cpucb.lapic_base & 0xfffff000) + reg);
}

static void
lapic_write(kcpu_t *cpu, uint32_t reg, uint32_t val)
{
	volatile uint32_t *addr = (volatile uint32_t *)P2V(
	    (cpu->cpucb.lapic_base & 0xfffff000) + reg);
	*addr = val;
}

void
lapic_eoi()
{
	lapic_write(curcpu(), kLAPICRegEOI, 0x0);
}

void
lapic_enable(uint8_t spurvec)
{
	ipl_t ipl = spldpc();
	lapic_write(curcpu(), kLAPICRegSpurious,
	    lapic_read(curcpu(), kLAPICRegSpurious) | (1 << 8) | spurvec);
	splx(ipl);
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

	lapic_write(curcpu(), kLAPICRegTimerDivider, 0x2); /* divide by 8*/
	lapic_write(curcpu(), kLAPICRegTimer, kIntVecLAPICTimer);

	pit_init_oneshot(hz);
	lapic_write(curcpu(), kLAPICRegTimerInitial, initial);

	pit_await_oneshot();
	apic_after = lapic_read(curcpu(), kLAPICRegTimerCurrentCount);
#if 0
	lapic_write(kLAPICRegTimer, 0x10000); /* disable*/
#endif

	ke_spinlock_release(&calib, ipl);

	return (initial - apic_after) * hz;
}

void
lapic_timer_start()
{
	ipl_t ipl = spldpc();
	lapic_write(curcpu(), kLAPICRegTimer,
	    kLAPICTimerPeriodic | kIntVecLAPICTimer);
	lapic_write(curcpu(), kLAPICRegTimerInitial,
	    curcpu()->cpucb.lapic_tps / KERN_HZ);
	splx(ipl);
}

static void
send_ipi(uint32_t lapic_id, uint8_t intr)
{
	lapic_write(curcpu(), kLAPICRegICR1, lapic_id << 24);
	lapic_write(curcpu(), kLAPICRegICR0, intr);
	while (lapic_read(curcpu(), kLAPICRegICR0) & 1 << 12)
		;
}

void
md_send_invlpg_ipi(kcpu_t *cpu)
{
	send_ipi(cpu->cpucb.lapic_id, kIntVecIPIInvlPG);
}

void md_send_dpc_ipi(kcpu_t *cpu)
{
	cpu->dpc_int = true;
	send_ipi(cpu->cpucb.lapic_id, kIntVecDPC);
}

void md_raise_dpc_interrupt()
{
	curcpu()->dpc_int = true;
	send_ipi(curcpu()->cpucb.lapic_id, kIntVecDPC);
}
