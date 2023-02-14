#include "hl/hl.h"
#include "ke/ke.h"

int64_t
ke_get_ticks(kcpu_t *cpu)
{
	return cpu->ticks;
}

void
ki_timer_enqueue(ktimer_t *callout)
{
	struct ktimer_queue *queue;
	ktimer_t *co;
	kcpu_t *cpu;
	ipl_t ipl;

#if DEBUG_TIMERS == 1
	ke_dbg(" -- callouts: Enqueuing %s for %lu ns\n", callout->name,
	    callout->nanosecs);
#endif

	ipl = ke_acquire_dpc_lock();
	cpu = hl_curcpu();

	kassert(callout->state == kTimerDisabled);
	kassert(callout->nanosecs > 0);
	callout->deadline = callout->nanosecs + cpu->ticks;
	queue = &cpu->timer_queue;

	TAILQ_FOREACH (co, queue, queue_entry) {
		if (co->deadline > callout->deadline) {
			TAILQ_INSERT_BEFORE(co, callout, queue_entry);
			goto next;
		}
	}
	/* no callouts or it's the longest till elapsing */
	TAILQ_INSERT_TAIL(queue, callout, queue_entry);

next:
#if 0
	ke_dbg("---------timer list-----------\n") uint64_t cum = 0;
	TAILQ_FOREACH (co, &cpu->callout_queue, queue_entry) {
		cum += co->nanosecs;
		ke_dbg("%s (%lumillis/cum %lumillis)\n", co->name,
		    co->nanosecs / (NS_PER_S / 1000), cum / (NS_PER_S / 1000));
	}
	ke_dbg("---------ends-----------\n")
#endif
	callout->state = kTimerPending;
	callout->cpu = cpu;

	ke_release_dpc_lock(ipl);
}

void
ki_timer_dequeue(ktimer_t *callout)
{
	struct ktimer_queue *queue;
	kcpu_t *cpu;
	ipl_t ipl;

	ipl = ke_acquire_dpc_lock();
	cpu = hl_curcpu();

#if DEBUG_TIMERS == 1
	ke_dbg(" -- removing callout %s\n", callout->name);
#endif

	kassert(callout->state == kTimerPending);
	cpu = callout->cpu;
	queue = &cpu->timer_queue;

	callout->state = kTimerDisabled;
	TAILQ_REMOVE(queue, callout, queue_entry);

	ke_release_dpc_lock(ipl);
}

bool
ki_cpu_hardclock(hl_intr_frame_t *frame, void *arg)
{
	struct ktimer_queue *queue = &hl_curcpu()->timer_queue;
	ktimer_t *co;
	uint64_t ticks;
	kcpu_t *cpu = hl_curcpu();
	ipl_t ipl;

	/* we are actually already at spl high */
	ipl = ke_acquire_dpc_lock();
	ticks = atomic_fetch_add(&cpu->ticks, NS_PER_S / KERN_HZ);

	if (cpu->current_thread->timeslice-- <= 0) {
		cpu->reschedule_reason = kRescheduleReasonPreempted;
		ki_raise_dpc_interrupt();
	}

	while (true) {
		co = TAILQ_FIRST(queue);

		if (co == NULL || co->deadline > ticks)
			goto next;

#if DEBUG_TIMERS == 1
		ke_dbg(" -- callouts: callout %s happened\n", co->name);
#endif

		TAILQ_REMOVE(queue, co, queue_entry);
		/* ! do we want kCalloutElapsed? Do we need it? */
		co->state = kTimerDisabled;

		ke_dpc_enqueue(&co->dpc);
	}

next:
	ke_release_dpc_lock(ipl);

	return true;
}
