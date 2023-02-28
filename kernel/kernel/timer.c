/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Jan 17 2023.
 */

#include "kernel/ke_internal.h"

void
timer_dpc(void *arg)
{
	ktimer_t *timer = arg;

	ipl_t ipl = ke_acquire_dispatcher_lock();

	timer->hdr.signalled = 1;

	kwaitblock_t *block = TAILQ_FIRST(&timer->hdr.waitblock_queue);
	while ((block) != NULL) {
		kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
		ki_waiter_maybe_wakeup(block->thread, &timer->hdr);
		block = next;
	}

	ke_release_dispatcher_lock(ipl);
}

void
ke_timer_init(ktimer_t *timer)
{
	timer->hdr.type = kDispatchTimer;
	timer->hdr.signalled = 0;
	timer->state = kTimerDisabled;
	timer->cpu = NULL;
	timer->dpc.arg = timer;
	timer->dpc.callback = timer_dpc;
	TAILQ_INIT(&timer->hdr.waitblock_queue);
}

void
ke_timer_set(ktimer_t *timer, uint64_t nanosecs)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	if (timer->state != kTimerPending) {
		ki_timer_dequeue(timer);
	}

	timer->nanosecs = nanosecs;
	timer->hdr.signalled = 0;
	ki_timer_enqueue(timer);

	ke_release_dispatcher_lock(ipl);
}
