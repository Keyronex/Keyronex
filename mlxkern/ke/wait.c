#include "ke/ke.h"

/*! @pre dispatcher_lock held */
static void
waiter_wake(kthread_t *thread, kwaitstatus_t result)
{
	kassert(thread->state == kThreadStateWaiting);
	thread->stats.total_wait_time += MAX2(
	    (ke_get_ticks(thread->cpu) - thread->stats.last_start_time), 1);
	thread->wait_result = result;
	TAILQ_INSERT_HEAD(&thread->cpu->runqueue, thread, runqueue_link);
	if (thread->cpu == hl_curcpu()) {
		thread->cpu->reschedule_reason = kRescheduleReasonPreempted;
		ki_raise_dpc_interrupt();
	} else {
		hl_ipi_reschedule(thread->cpu);
	}
}