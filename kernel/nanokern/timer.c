/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Jan 17 2023.
 */

#include "ki.h"

void
timer_dpc(void *arg)
{
	ktimer_t *timer = arg;
	kwaitblock_queue_t queue = TAILQ_HEAD_INITIALIZER(queue);
	ipl_t ipl = ke_spinlock_acquire(&timer->hdr.spinlock);

	timer->hdr.signalled = 1;
	ki_signal(&timer->hdr, &queue);
	ke_spinlock_release_nospl(&timer->hdr.spinlock);

	ke_acquire_scheduler_lock();
	ki_wake_waiters(&queue);
	ke_release_scheduler_lock(ipl);
}

void
ke_timer_init(ktimer_t *timer)
{
	timer->hdr.type = kDispatchTimer;
	timer->hdr.signalled = 0;
	ke_spinlock_init(&timer->hdr.spinlock);
	timer->pending = false;
	timer->cpu = NULL;
	timer->dpc.arg = timer;
	timer->dpc.callback = timer_dpc;
	timer->dpc.cpu = NULL;
	TAILQ_INIT(&timer->hdr.waitblock_queue);
}

void
ke_timer_set(ktimer_t *timer, uint64_t nanosecs)
{
	ipl_t ipl = ke_spinlock_acquire(&timer->hdr.spinlock);

	if (timer->pending)
		ki_timer_dequeue(timer);

	timer->nanosecs = nanosecs;
	timer->hdr.signalled = 0;
	ki_timer_enqueue(timer);

	ke_spinlock_release(&timer->hdr.spinlock, ipl);
}

void
ke_timer_cancel(ktimer_t *timer)
{
	ipl_t ipl = ke_spinlock_acquire(&timer->hdr.spinlock);

	/* todo: no attempt is made to wait for or cancel the associated DPC */

	if (timer->pending)
		ki_timer_dequeue(timer);

	kassert(timer->dpc.cpu == NULL);

	ke_spinlock_release(&timer->hdr.spinlock, ipl);
}
