/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Tue Aug 06 2024.
 */

#include "kdk/kern.h"
#include "kdk/riscv64.h"
#include "kern/ki.h"
#include "vm/vmp.h"

void
c_trap(md_intr_frame_t *frame)
{

	if (frame->usermode)
		kfatal("Usermode Trap: "
			      "Frame %p, sepc 0x%zx, sstatus 0x%zx, "
			      "scause 0x%zx, stval 0x%zx\n",
		    frame, frame->sepc, frame->sstatus, frame->scause,
		    frame->stval);

	switch (frame->scause & 0x7fffffff) {
	case 9: {
		void aplic_irq(md_intr_frame_t * frame);
		return aplic_irq(frame);
	}

	case 12:
	case 13:
	case 15: {
		bool instr = false, write = false;

		ki_set_interrupts(1);

		if (frame->scause == 12)
			instr = true;
		else if (frame->scause == 15)
			write = true;

		(void)instr;

		vmp_fault(frame, frame->stval, write, NULL);

		ki_disable_interrupts();

		return;
	}

	default:
		kprintf_nospl("Unexpected Trap: "
			      "Frame %p, sepc 0x%zx, sstatus 0x%zx, "
			      "scause 0x%zx, stval 0x%zx\n",
		    frame, frame->sepc, frame->sstatus, frame->scause,
		    frame->stval);
		for (;;)
			;
	}
}
