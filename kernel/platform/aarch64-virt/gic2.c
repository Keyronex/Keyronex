#include "kdk/kern.h"
#include "kdk/vm.h"

/* GIC stuff */
vaddr_t gicd_base = 0x0;
static uintptr_t timer_hz;

enum {
	kGICD_CTLR = 0x0000,
	kGICD_ISENABLER = 0x0100,
	kGICD_IPRIORITYR = 0x0400,
	kGICD_ITARGETSR = 0x0800,
	kGICD_ICFGR = 0x0c00,
};

enum {
	kGICC_CTLR = 0x0000,
	kGICC_PMR = 0x0004,
	kGICC_IAR = 0x000c,
	kGICC_EOIR = 0x0010,
};

void
reset_timer(void)
{
	uint64_t deadline;
	asm volatile("mrs %0, cntpct_el0" : "=r"(deadline));
	deadline += timer_hz / KERN_HZ;
	asm volatile("msr cntp_cval_el0, %0" ::"r"(deadline));
}

uint32_t
gicd_read(uint32_t reg)
{
	return *(volatile uint32_t *)(gicd_base + reg);
}

void
gicd_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(gicd_base + reg) = value;
}

void
gicd_write_ctlr(uint32_t value)
{
	gicd_write(0, value);
}

void
gicd_write_isenabler(int gsiv, uint32_t enabled)
{
	uint32_t reg_offset = kGICD_ISENABLER + (gsiv / 32) * 4;
	uint32_t bit_mask = 1 << (gsiv % 32);
	uint32_t value = gicd_read(reg_offset);
	value |= bit_mask;
	gicd_write(reg_offset, value);
}

uint32_t
gicc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(curcpu()->cpucb.gicc_base + reg);
}

void
gicc_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(curcpu()->cpucb.gicc_base + reg) = value;
}

void
gicc_write_ctlr(uint32_t value)
{
	gicc_write(0, value);
}

uint32_t
gengic_acknowledge(void)
{
	return gicc_read(kGICC_IAR);
}

void
gengic_eoi(uint32_t intr)
{
	gicc_write(kGICC_EOIR, intr);
}

void
gengic_dist_setedge(uint32_t gsi, bool edge)
{
	uint32_t reg_offset = kGICD_ICFGR + (gsi / 16) * 4;
	uint32_t bit_shift = (gsi % 16) * 2;
	uint32_t value = *(volatile uint32_t *)(gicd_base + reg_offset);

	if (edge)
		value |= (0x2 << bit_shift);
	else
		value &= ~(0x2 << bit_shift);

	*(volatile uint32_t *)(gicd_base + reg_offset) = value;
}

void
gengic_dist_settarget(uint32_t gsi, uint64_t target)
{
	uint32_t reg_offset = kGICD_ITARGETSR + (gsi / 4) * 4;
	uint32_t bit_shift = (gsi % 4) * 8;
	uint32_t value = *(volatile uint32_t *)(gicd_base + reg_offset);

	value &= ~(0xFF << bit_shift);
	value |= ((target & 0xFF) << bit_shift);

	*(volatile uint32_t *)(gicd_base + reg_offset) = value;
}

void
gengic_dist_setenabled(uint32_t gsi)
{
#if 0
	uint32_t priority_reg = kGICD_IPRIORITYR + (gsi / 4) * 4;
	uint32_t priority_shift = (gsi % 4) * 8;
	gicd_write(priority_reg,
	    ((*(volatile uint32_t *)(gicd_base + priority_reg)) &
		~(0xFF << priority_shift)) |
		(0x80 << priority_shift));
#endif
	gicd_write_isenabler(gsi, 1);
}

void
enable_timer(void)
{
#if 1
	asm volatile("mrs %0, cntfrq_el0" : "=r"(timer_hz));
	kprintf("ticks per second = %lu\n", timer_hz);

	asm volatile("msr cntp_cval_el0, %0" ::"r"((uint64_t)-1));
	asm volatile("msr cntp_ctl_el0, %0" ::"r"((uint64_t)0b01));

	reset_timer();
	kprintf("Enabling ctlr...\n");
#endif
	gicd_write_ctlr(1);
	gicc_write_ctlr(1);
	gicc_write(kGICC_PMR, 0xff);

	gicd_write_isenabler(30, 1);

	asm volatile("msr daifclr, #0x2");

	// asm volatile("msr icc_igrpen1_el1, %0" ::"r"((uint32_t)1));
	// asm volatile("msr icc_igrpen0_el1, %0" ::"r"((uint32_t)1));
}
