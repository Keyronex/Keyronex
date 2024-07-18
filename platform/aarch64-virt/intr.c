#include <stdint.h>

#include "kdk/kern.h"

static uint64_t timer_hz;

static inline void
write_vbar_el1(void *addr)
{
	asm volatile("msr VBAR_EL1, %0" ::"r"(addr));
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
	kfatal("el1t intr\n");
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
	kfatal("el1 intr\n");
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
reset_timer(void)
{
	uint64_t deadline;
	asm volatile("mrs %0, cntpct_el0" : "=r"(deadline));
	deadline += timer_hz / KERN_HZ;
	asm volatile("msr cntp_cval_el0, %0" ::"r"(deadline));
	asm volatile("msr daifclr, #0x2");
}

void
intr_setup(void)
{
	extern void *vectors;
	write_vbar_el1(&vectors);

		asm volatile("mrs %0, cntfrq_el0" : "=r"(timer_hz));
		kprintf("ticks per second = %lu\n", timer_hz);

		asm volatile("msr cntp_cval_el0, %0" ::"r"((uint64_t)-1));
		asm volatile("msr cntp_ctl_el0, %0" ::"r"((uint64_t)0b11));

		reset_timer();

		//asm volatile("msr icc_igrpen1_el1, %0" ::"r"((uint32_t)1));
		//asm volatile("msr icc_igrpen0_el1, %0" ::"r"((uint32_t)1));
}
