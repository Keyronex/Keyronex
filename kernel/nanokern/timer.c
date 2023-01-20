#include <nanokern/thread.h>

void
timer_dpc(void *arg)
{
	ktimer_t *timer = arg;

	ipl_t ipl = nk_spinlock_acquire(&nk_lock);

	timer->hdr.signalled = 1;

	kwaitblock_t *block = TAILQ_FIRST(&timer->hdr.waitblock_queue);
	while ((block) != NULL) {
		kwaitblock_t *next = TAILQ_NEXT(block, queue_entry);
		nkx_waiter_maybe_wakeup(block->thread, &timer->hdr);
		block = next;
	}

	nk_spinlock_release(&nk_lock, ipl);
}

void
nk_timer_init(ktimer_t *timer)
{
	timer->hdr.type = kDispatchTimer;
	timer->hdr.signalled = 0;
	timer->callout.name = "ktimer_callout";
	timer->callout.cpu = NULL;
	timer->callout.dpc.arg = timer;
	timer->callout.dpc.bound = false;
	timer->callout.dpc.callback = timer_dpc;
	TAILQ_INIT(&timer->hdr.waitblock_queue);
}

void
nk_timer_set(ktimer_t *timer, uint64_t nanosecs)
{
	ipl_t ipl = nk_spinlock_acquire(&nk_lock);

	if (timer->callout.state != kCalloutDisabled) {
		nkx_callout_dequeue(&timer->callout);
	}

	timer->callout.nanosecs = nanosecs;
	timer->hdr.signalled = 0;
	nkx_callout_enqueue(&timer->callout);

	nk_spinlock_release(&nk_lock, ipl);
}
