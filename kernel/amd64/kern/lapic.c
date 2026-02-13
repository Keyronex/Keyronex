/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file lapic.c
 * @brief Local APIC functionality.
 */

#include <keyronex/cpu.h>
#include <keyronex/dlog.h>
#include <keyronex/x86.h>
#include <keyronex/vm.h>

#include <asm/io.h>

enum {
	LAPIC_REG_EOI = 0xb0,
	LAPIC_REG_SPURIOUS = 0xf0,
	LAPIC_REG_ICR0 = 0x300,
	LAPIC_REG_ICR1 = 0x310,
	LAPIC_REG_TIMER = 0x320,
	LAPIC_REG_TIMER_INITIAL = 0x380,
	LAPIC_REG_TIMER_CURRENTCOUNT = 0x390,
	LAPIC_REG_TIMER_DIVIDER = 0x3e0,
};

enum {
	kLAPICTimerPeriodic = 0x20000,
	kLAPICTimerTSCDeadline = 0x40000,
};

uint64_t timebase;
vaddr_t lapic_vbase;

void
ke_arch_pause(void)
{
	asm("pause");
}

kabstime_t
ke_time(void)
{
	uint64_t tsc = __builtin_ia32_rdtsc();
	uint64_t secs = tsc / timebase;
	uint64_t nanosecs = tsc % timebase;

	return secs * NS_PER_S + (nanosecs * NS_PER_S) / timebase;
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

static uint32_t
lapic_read(uint32_t reg)
{
	return *(volatile uint32_t *)(lapic_vbase + reg);
}

static void
lapic_write(uint32_t reg, uint32_t val)
{
	volatile uint32_t *addr = (volatile uint32_t *)(lapic_vbase + reg);
	*addr = val;
}


	static kspinlock_t calib = KSPINLOCK_INITIALISER;

void
lapic_early_init(void)
{
	uint64_t tsc_start, tsc_end;
	ipl_t ipl = ke_spinlock_enter(&calib);

	pit_init_oneshot(25);
	tsc_start = __builtin_ia32_rdtsc();
	pit_await_oneshot();
	tsc_end = __builtin_ia32_rdtsc();

	timebase = ((tsc_end - tsc_start) * 25);

	lapic_vbase = p2v(rdmsr(IA32_APIC_BASE_MSR) & 0xfffff000);
	ke_spinlock_exit(&calib, ipl);
}

uint32_t
lapic_timer_calibrate(void)
{
	const uint32_t initial = 0xffffffff;
	const uint32_t hz = 50;
	uint32_t apic_after;
	ipl_t ipl = ke_spinlock_enter(&calib);

	lapic_write(LAPIC_REG_TIMER_DIVIDER, 0x2); /* divide by 8*/
	lapic_write(LAPIC_REG_TIMER, 224);

	pit_init_oneshot(hz);
	lapic_write(LAPIC_REG_TIMER_INITIAL, initial);

	pit_await_oneshot();
	apic_after = lapic_read(LAPIC_REG_TIMER_CURRENTCOUNT);
#if 0
	lapic_write(kLAPICRegTimer, 0x10000); /* disable*/
#endif

	ke_spinlock_exit(&calib, ipl);

	return (initial - apic_after) * hz;
}

void
lapic_cpu_init(void)
{
	lapic_write(LAPIC_REG_SPURIOUS, (1 << 8) | 255);
}

void
lapic_eoi(void)
{
	lapic_write(LAPIC_REG_EOI, 0x0);
}

void
lapic_timer_start(void)
{
	lapic_write(LAPIC_REG_TIMER, kLAPICTimerPeriodic | 225);
	lapic_write(LAPIC_REG_TIMER_INITIAL,
	    CPU_LOCAL_LOAD(arch.lapic_tps) / KERN_HZ);
}

void
ke_platform_start_dispatching(void)
{
	lapic_early_init();
	lapic_cpu_init();
	CPU_LOCAL_STORE(arch.lapic_tps, 0);
	/* measure thrice & average it */
	for (int i = 0; i < 3; i++)
		CPU_LOCAL_STORE(arch.lapic_tps,
		    CPU_LOCAL_LOAD(arch.lapic_tps) +
			lapic_timer_calibrate() / 3);
	lapic_timer_start();
	asm("sti");
}

void
kep_arch_ipi_broadcast(void)
{
	uint32_t icr_low = 225 | (0 << 8) | (0 << 11) | (1 << 14) | (3 << 18);

	lapic_write(LAPIC_REG_ICR0, icr_low);
}

void
kep_arch_ipi_unicast(uint32_t cpu_num)
{
	uint8_t lapic_id = ke_cpu_data[cpu_num]->arch.lapic_id;
	uint32_t icr_low = 225 | (0 << 8) | (0 << 11) | (1 << 14);
	uint32_t icr_high = ((uint32_t)lapic_id) << 24;
	lapic_write(LAPIC_REG_ICR1, icr_high);
	lapic_write(LAPIC_REG_ICR0, icr_low);
}
