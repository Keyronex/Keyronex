
#include "executive/exp.h"
#include "kdk/aarch64.h"
#include "kdk/kern.h"
#include "kdk/libkern.h"
#include "vm/vmp.h"

void plat_irq(md_intr_frame_t *frame);

static inline void
write_vbar_el1(void *addr)
{
	asm volatile("msr VBAR_EL1, %0" ::"r"(addr));
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

void
common_sync(md_intr_frame_t *frame)
{
	struct aarch64_esr esr;
	bool userland = false;

	ki_set_interrupts(true);

	memcpy(&esr, &frame->esr, sizeof(struct aarch64_esr));

	switch (esr.generic.EC) {
	case 0x15: /* svc*/ {
		frame->x0 = ex_syscall_dispatch(frame->x0, frame->x1, frame->x2,
		    frame->x3, frame->x4, frame->x5, frame->x6, &frame->x1);
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

		(void)insn;
		(void)userland;

		vmp_fault(frame, frame->far, write, NULL);

		break;
	}

	default:
		kfatal("el1 sync: frame %p, elr 0x%zx, far 0x%zx, spsr 0x%zx, "
		       "esr 0x%zx; esr->ec = 0x%x\n",
		    frame, frame->elr, frame->far, frame->spsr, frame->esr,
		    esr.generic.EC);
	}
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
	kfatal("el1 error\n");
}

void
c_el0_sync(md_intr_frame_t *frame)
{
	common_sync(frame);
}

void
c_el0_intr(md_intr_frame_t *frame)
{
	c_el1_intr(frame);
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
