/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 15 2023.
 */

#include "ke/ke.h"

void
ki_thread_start(kthread_t *thread)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	TAILQ_INSERT_HEAD(&thread->cpu->runqueue, thread, runqueue_link);

	if (thread->cpu == hl_curcpu()) {
		hl_curcpu()->reschedule_reason = kRescheduleReasonPreempted;
		ki_raise_dpc_interrupt();
	} else {
		kfatal("un-handled\n");
		//md_ipi_reschedule(thread->cpu);
	}

	ke_release_dispatcher_lock(ipl);
}
