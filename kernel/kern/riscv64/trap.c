/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Tue Aug 06 2024.
 */

#include "kdk/kern.h"

void
c_trap(md_intr_frame_t *frame)
{
	kprintf("Trap: "
		"Frame %p, sepc 0x%zx, sstatus 0x%zx, "
		"scause 0x%zx, stval 0x%zx\n",
	    frame, frame->sepc, frame->sstatus, frame->scause, frame->stval);
	for (;;)
		;
}
