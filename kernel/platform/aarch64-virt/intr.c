#include <stdint.h>

#include "kdk/aarch64.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "kern/ki.h"

static uint64_t timer_hz;

static inline void
write_vbar_el1(void *addr)
{
	asm volatile("msr VBAR_EL1, %0" ::"r"(addr));
}

/* GIC stuff */
uintptr_t gicd_base = 0x0;

enum {
	kGICD_CTLR = 0x0000,
	kGICD_ISENABLER = 0x0100,
};

enum {
	kGICC_CTLR = 0x0000,
	kGICC_PMR = 0x0004,
	kGICC_IAR = 0x000c,
	kGICC_EOIR = 0x0010,
};

void
gicd_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(gicd_base + reg) = value;
}

void gicd_write_ctlr(uint32_t value)
{
	gicd_write(0, value);
}

void
gicd_write_isenabler(int gsiv, uint32_t value)
{
	gicd_write(kGICD_ISENABLER + gsiv / 32, 1 << (gsiv % 32));
}

uint32_t gicc_read(uint32_t reg)
{
	return *(volatile uint32_t *)(curcpu()->cpucb.gicc_base + reg);
}

void
gicc_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(curcpu()->cpucb.gicc_base + reg) = value;
}

void gicc_write_ctlr(uint32_t value)
{
	gicc_write(0, value);
}

/* Ends GIC stuff */


void
reset_timer(void)
{
	uint64_t deadline;
	asm volatile("mrs %0, cntpct_el0" : "=r"(deadline));
	deadline += timer_hz / 4;
	asm volatile("msr cntp_cval_el0, %0" ::"r"(deadline));
}

void
c_exception(md_intr_frame_t *frame)
{
	kfatal("exception\n");
}

void
c_el1t_sync(md_intr_frame_t *frame)
{
	kfatal("el1 T sync: frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, esr 0x%zx\n",  frame, frame->elr,
	    frame->far, frame->spsr, frame->esr);
}

void
c_el1t_irq(md_intr_frame_t *frame)
{
	kfatal("el1t irq\n");
}

void
c_el1t_fiq(md_intr_frame_t *frame)
{
	kfatal("el1t fiq\n");
}

void
c_el1t_error(md_intr_frame_t *frame)
{
	kfatal("el1t error\n");
}

void
c_el1_sync(md_intr_frame_t *frame)
{
	kfatal("el1 sync: frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, esr 0x%zx\n", frame, frame->elr,
	    frame->far, frame->spsr, frame->esr);
}

void
c_el1_intr(md_intr_frame_t *frame)
{
	uint32_t intr = gicc_read(kGICC_IAR);
	kprintf("el1 intr: IAR = %u\n",intr );
	reset_timer();
	gicc_write(kGICC_EOIR, intr);
}

void
c_el1_fiq(md_intr_frame_t *frame)
{
	kfatal("el1 fiq\n");
}

void
c_el1_error(md_intr_frame_t *frame)
{
	kfatal("el1 error\n");
}

void
c_el0_sync(md_intr_frame_t *frame)
{
	kfatal("el0 sync\n");
}

void
c_el0_intr(md_intr_frame_t *frame)
{
	kfatal("intr\n");
}

void
c_el0_fiq(md_intr_frame_t *frame)
{
	kfatal("el0 fiq\n");
}

void
c_el0_error(md_intr_frame_t *frame)
{
	kfatal("el0 error\n");
}

void
intr_init(void)
{
	extern void *vectors;
	write_vbar_el1(&vectors);
}

bool
ki_disable_interrupts(void)
{
	uint64_t daif;
	asm volatile("mrs %0, daif" : "=r"(daif));
	asm volatile("msr daifset, #0xf");
	return (daif & 0xf) == 0;
}

void
ki_set_interrupts(bool enable)
{
	if (enable)
		asm volatile("msr daifclr, #0xf");
	else
		asm volatile("msr daifset, #0xf");
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

#if 0
		asm volatile("mrs %0, cntfrq_el0" : "=r"(timer_hz));
		kprintf("ticks per second = %lu\n", timer_hz);

		asm volatile("msr cntp_cval_el0, %0" ::"r"((uint64_t)-1));
		asm volatile("msr cntp_ctl_el0, %0" ::"r"((uint64_t)0b01));

		reset_timer();
		kprintf("Enabling ctlr...\n");

		gicd_write_ctlr(1);
		gicc_write_ctlr(1);
		gicc_write(kGICC_PMR, 0xff);

		gicd_write_isenabler(30, 1);

		asm volatile("msr daifclr, #0x2");

		//asm volatile("msr icc_igrpen1_el1, %0" ::"r"((uint32_t)1));
		//asm volatile("msr icc_igrpen0_el1, %0" ::"r"((uint32_t)1));
#endif
