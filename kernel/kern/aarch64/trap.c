
#include "executive/exp.h"
#include "kdk/aarch64.h"
#include "kdk/kern.h"
#include "kdk/libkern.h"
#include "vm/vmp.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

struct __attribute__((packed)) aarch64_esr {
	union {
		struct {
			uintptr_t ISS : 25; /* Instruction Specific Syndrome */
			uintptr_t IL : 1;   /* Instruction Length */
			uintptr_t EC : 6;   /* Exception Class */
		} __attribute__((packed)) generic;
		struct {
			uintptr_t DFSC : 6;    /* Data Fault Status Code */
			uintptr_t WnR : 1;     /* Write not Read */
			uintptr_t S1PTW : 1;   /* Stage 1 Page Table Walk */
			uintptr_t CM : 1;      /* Cache Maintenance */
			uintptr_t EA : 1;      /* External Abort */
			uintptr_t FnV : 1;     /* FAR not Valid */
			uintptr_t RES0 : 2;    /* Reserved */
			uintptr_t VNCR : 1;    /* VNCR bit */
			uintptr_t RES0_1 : 10; /* Reserved */
			uintptr_t ISV : 1;     /* Instruction Syndrome Valid */
			uintptr_t IL : 1;      /* Instruction Length */
			uintptr_t EC : 6;      /* Exception Class */
		} __attribute__((packed)) DFSC;
		struct {
			uintptr_t IFSC : 6;  /* Instruction Fault Status Code */
			uintptr_t RES0 : 1;  /* Reserved */
			uintptr_t S1PTW : 1; /* Stage 1 Page Table Walk */
			uintptr_t CM : 1;    /* Cache Maintenance */
			uintptr_t EA : 1;    /* External Abort */
			uintptr_t FnV : 1;   /* FAR not Valid */
			uintptr_t RES0_1 : 2;  /* Reserved */
			uintptr_t VNCR : 1;    /* VNCR bit */
			uintptr_t RES0_2 : 10; /* Reserved */
			uintptr_t ISV : 1;     /* Instruction Syndrome Valid */
			uintptr_t IL : 1;      /* Instruction Length */
			uintptr_t EC : 6;      /* Exception Class */
		} __attribute__((packed)) IFSC;
	};
	uintptr_t ISS2 : 5;  /* Instruction Specific Syndrome 2 */
	uintptr_t RES0 : 27; /* Reserved */
};

static_assert(sizeof(struct aarch64_esr) == sizeof(void *), "ESR wrong size!");

void plat_irq(md_intr_frame_t *frame);

static inline void
write_vbar_el1(void *addr)
{
	asm volatile("msr VBAR_EL1, %0" ::"r"(addr));
}

int
ki_disable_interrupts(void)
{
	uint64_t daif;
	asm volatile("mrs %0, daif" : "=r"(daif));
	asm volatile("msr daifset, #0xf");
	return (daif & 0xf) == 0;
}

void
ki_set_interrupts(int enable)
{
	if (enable)
		asm volatile("msr daifclr, #0xf");
	else
		asm volatile("msr daifset, #0xf");
}


static uint64_t unkesr_counter[64];

void
common_sync(md_intr_frame_t *frame)
{
	struct aarch64_esr esr;
	bool userland = false;

	if (frame->esr == 0x2000000) {
		kfatal("Unknown ESR!!\n");

		/*
		 * Old note below. For now, we treat it as a fatal error again,
		 * as I have not been able to reproduce it recently.
		 *
		 * ---
		 *
		 * I don't know why this happens nor why tlb invalidation gets
		 * it going again.
		 *
		 * TLB entries are invalidated apparently in all the right
		 * places, and even adding tlbi vmalle1 all over the place
		 * does not fix things.
		 *
		 * Even with aggressive page stealing, no harm seems to
		 * happen... for whatever reason, this nonsense somehow gets
		 * userland execution going again.
		 *
		 * The counter is kept for ease of investigation in the future.
		 * A per-thread counter ought to be kept also and repeated
		 * entries here for the same ELR should terminate the thread.
		 */
		unkesr_counter[(frame->elr >> 4) % 64]++;
		asm volatile("tlbi vaae1, %0\n\t"
			     :
			     : "r"(frame->elr >> 12)
			     : "memory");
		return;
	}

	ki_set_interrupts(true);

	memcpy(&esr, &frame->esr, sizeof(struct aarch64_esr));

	switch (esr.generic.EC) {
	case 0x15: /* svc*/ {
		frame->x0 = ex_syscall_dispatch(frame, frame->x0, frame->x1,
		    frame->x2, frame->x3, frame->x4, frame->x5, frame->x6,
		    &frame->x1);
		break;
	}

	case 0x20: /* insn abort, lower EL */
	case 0x24: /* data abort, lower EL */
		userland = true;
		/* fallthrough */

	case 0x21: /* insn abort */
	case 0x25: /* data abort */ {
		bool insn;
		bool write;

		if (esr.generic.EC & 0x4) {
			insn = false;
			write = esr.DFSC.WnR;
		} else {
			insn = true;
			write = false;
		}

		(void)userland;

		vmp_fault(frame, frame->far, write, insn, userland, NULL);

		break;
	}

	default:
		ki_disable_interrupts();
		splraise(kIPLHigh);
		kfatal("el1 sync: frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, "
		       "esr 0x%zx; esr->ec = 0x%x\n",
		    frame, frame->elr, frame->far, frame->spsr, frame->esr,
		    esr.generic.EC);
	}

	ki_disable_interrupts();
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
	common_sync(frame);
}

void
c_el1_intr(md_intr_frame_t *frame)
{
	plat_irq(frame);
}

void
c_el1_fiq(md_intr_frame_t *frame)
{
	kfatal("el1 fiq\n");
}

void
c_el1_error(md_intr_frame_t *frame)
{
	kfatal("el1 error: "
	       "frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, esr 0x%zx\n",
	    frame, frame->elr, frame->far, frame->spsr, frame->esr);
}

void
c_el0_sync(md_intr_frame_t *frame)
{
	common_sync(frame);
}

void
c_el0_intr(md_intr_frame_t *frame)
{
	plat_irq(frame);
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
