/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file lapic.c
 * @brief Local APIC functionality.
 */

#include <sys/k_cpu.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/vm.h>
#include <sys/x86.h>

#include <asm/io.h>
#include <kern/defs.h>

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

	pit_init_oneshot(25);
	tsc_start = __builtin_ia32_rdtsc();
	pit_await_oneshot();
	tsc_end = __builtin_ia32_rdtsc();

	timebase = ((tsc_end - tsc_start) * 25);

	lapic_vbase = p2v(rdmsr(IA32_APIC_BASE_MSR) & 0xfffff000);
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
	lapic_write(LAPIC_REG_TIMER, kLAPICTimerPeriodic | 224);
	lapic_write(LAPIC_REG_TIMER_INITIAL,
	    CPU_LOCAL_LOAD(arch.lapic_tps) / KERN_HZ);
}

void
ke_platform_early_init(void)
{
	lapic_early_init();
}


typedef struct {
	uint16_t length;
	uint16_t base_low;
	uint8_t base_mid;
	uint8_t access;
	uint8_t flags;
	uint8_t base_high;
	uint32_t base_upper;
	uint32_t reserved;
} __attribute__((packed)) tss_gdt_entry_t;

static struct gdt {
	uint64_t null;
	uint64_t code16;
	uint64_t data16;
	uint64_t code32;
	uint64_t data32;
	uint64_t code64;
	uint64_t data64;
	uint64_t code64_user;
	uint64_t data64_user;
	tss_gdt_entry_t tss;
} __attribute__((packed)) gdt = {
	.null = 0x0,
	.code16 = 0x8f9a000000ffff,
	.data16 = 0x8f92000000ffff,
	.code32 = 0xcf9a000000ffff,
	.data32 = 0xcf92000000ffff,
	.code64 = 0xaf9a000000ffff,
	.data64 = 0x8f92000000ffff,
	.code64_user = 0xaffa000000ffff,
	.data64_user = 0x8ff2000000ffff,
	.tss = { .length = 0x68, .access = 0x89 },
};

void
load_gdt()
{
	struct {
		uint16_t limit;
		vaddr_t addr;
	} __attribute__((packed)) gdtr = { sizeof(gdt) - 1, (vaddr_t)&gdt };

	asm volatile("lgdt %0" : : "m"(gdtr));
}

void
setup_cpu_gdt(void)
{
	static kspinlock_t gdt_lock = KSPINLOCK_INITIALISER;
	ipl_t ipl = ke_spinlock_enter(&gdt_lock);
	struct tss *tss = kmem_alloc(sizeof(struct tss));

	// kassert((uintptr_t)cpu->tss / PGSIZE ==
	//     ((uintptr_t)cpu->tss + 104) / PGSIZE);
	CPU_LOCAL_STORE(arch.tss, tss);

	gdt.tss.length = 0x68;
	gdt.tss.base_low = (uintptr_t)tss;
	gdt.tss.base_mid = (uintptr_t)tss >> 16;
	gdt.tss.access = 0x89;
	gdt.tss.flags = 0x0;
	gdt.tss.base_high = (uintptr_t)tss >> 24;
	gdt.tss.base_upper = (uintptr_t)tss >> 32;
	gdt.tss.reserved = 0x0;
	load_gdt();
	asm volatile("ltr %0" ::"rm"((uint16_t)offsetof(struct gdt, tss)));
	ke_spinlock_exit(&gdt_lock, ipl);
}


void
ke_platform_start_dispatching(void)
{
	/* enable SSE and SSE2 */
	uint64_t cr0, cr4;

	cr0 = read_cr0();
	cr0 &= ~((uint64_t)1 << 2);
	cr0 |= (uint64_t)1 << 1;
	write_cr0(cr0);

	cr4 = read_cr4();
	cr4 |= (uint64_t)3 << 9;
	write_cr4(cr4);

	setup_cpu_gdt();
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
