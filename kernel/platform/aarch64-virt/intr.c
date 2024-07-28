#include <stdint.h>

#include "gic.h"
#include "kdk/aarch64.h"
#include "kdk/kern.h"
#include "kdk/vm.h"
#include "kern/ki.h"

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
	kfatal(
	    "el1 T sync: frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, esr 0x%zx\n",
	    frame, frame->elr, frame->far, frame->spsr, frame->esr);
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
	kfatal(
	    "el1 sync: frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, esr 0x%zx\n",
	    frame, frame->elr, frame->far, frame->spsr, frame->esr);
}

void
c_el1_intr(md_intr_frame_t *frame)
{
	uint32_t intr = gengic_acknowledge();
	switch (intr) {
	case 30: {
		ipl_t ipl = splraise(kIPLHigh);
		void reset_timer(void);
		reset_timer();
		ki_cpu_hardclock(frame, curcpu());
		splx(ipl);
		break;
	}

	default:
		kfatal("el1 intr: IAR = %u\n", intr);
	}
	gengic_eoi(intr);
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
