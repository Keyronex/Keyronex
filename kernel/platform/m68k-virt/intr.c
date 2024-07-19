#include <keyronex/syscall.h>

#include "goldfish.h"
#include "kdk/m68k.h"
#include "kdk/kern.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "kern/ki.h"
#include "vm/vmp.h"
#include "executive/exp.h"

#pragma GCC diagnostic ignored "-Waddress-of-packed-member"

void
md_raise_dpc_interrupt(void)
{
	curcpu()->cpucb.dpc_int = true;
}

ipl_t
splraise(ipl_t to)
{
	ipl_t oldipl;
	if (to > kIPLDPC) {
		/* mask interrupts */
		asm volatile("ori %0, %%sr" ::"i"(7 << 8) : "memory");
	}
	oldipl = curcpu()->cpucb.ipl;
	curcpu()->cpucb.ipl = to;
	return oldipl;
}

/*!
 * @brief Lower the interrupt priority level.
 */
void
splx_internal(ipl_t to, bool do_dpcs, bool do_hardware)
{
	ipl_t oldipl = curcpu()->cpucb.ipl;
	kassert(to <= oldipl);
	if (oldipl > kIPLDPC && to <= kIPLDPC && do_hardware) {
		/* unmask interrupts */
		asm volatile("andi %0, %%sr" ::"i"(~(7 << 8)) : "memory");
	}
	if (to < kIPLDPC && do_dpcs) {
		curcpu()->cpucb.ipl = kIPLDPC;
		while (curcpu()->cpucb.dpc_int) {
			curcpu()->cpucb.dpc_int = 0;
			ki_dispatch_dpcs(curcpu());
		}
	}
	curcpu()->cpucb.ipl = to;
}

void
splx(ipl_t to)
{
	splx_internal(to, true, true);
}

ipl_t
splget(void)
{
	return curcpu()->cpucb.ipl;
}

void
c_exception(md_intr_frame_t *iframe)
{
	ipl_t ipl = curcpu()->cpucb.ipl;

	switch (iframe->vector_offset) {
	case 0x8: /* access error */
		vmp_fault(iframe, iframe->format_7.ea, !iframe->format_7.ssw.rw,
		    NULL);
		break;

	case 0x64: /* Level 1 autovector */
	case 0x68:
	case 0x6c:
	case 0x70:
	case 0x74:
	case 0x78:
	case 0x7c:
		curcpu()->cpucb.ipl = kIPLHigh;
		// pac_printf("interrupt on autovector %u\n",
		//     (iframe->vector_offset - 0x64) / 4);
		gfpic_dispatch((iframe->vector_offset - 0x64) / 4, iframe);
		break;

	case 0x80: /* trap #0 */
		iframe->d0 = ex_syscall_dispatch(iframe->d0, iframe->d1,
		    iframe->d2, iframe->d3, iframe->d4, iframe->d5, iframe->a0,
		    &iframe->d1);
		break;

	default:
		splraise(kIPLHigh);
		kfatal("unhandled interrupt (vector offset 0x%x, num. %u)\n",
		    iframe->vector_offset, iframe->vector_offset / 4);
	}

	splx_internal(ipl, true, false);
}

void
intr_init(void)
{
	static void *ivt[256];

	extern void *asm_exception;

	for (int i = 0; i < 256; i++)
		ivt[i] = &asm_exception;

	/* load VBR to show where IVT is at */
	asm volatile("move.c %0, %%vbr" ::"r"(ivt));
	/* hardware IPL to 0 */
	asm volatile("andi %0, %%sr" ::"i"(~(7 << 8)) : "memory");
}

void
md_intr_frame_trace(md_intr_frame_t *frame)
{
}
