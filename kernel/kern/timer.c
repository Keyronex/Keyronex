/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Jan 17 2023.
 */

#include "kdk/kern.h"
#include "ki.h"


void
ke_timer_init(ktimer_t *timer)
{
	timer->hdr.type = kDispatchTimer;
	timer->hdr.signalled = 0;
	ke_spinlock_init(&timer->hdr.spinlock);
	__atomic_store_n(&timer->state,  kTimerDisabled, __ATOMIC_RELAXED);
	timer->cpu = NULL;
	timer->dpc = NULL;
	TAILQ_INIT(&timer->hdr.waitblock_queue);
}

void
ke_timer_set(ktimer_t *timer, uint64_t nanosecs)
{
	ipl_t ipl = spldpc();

	ke_spinlock_acquire_nospl(&timer->hdr.spinlock);

retry:
	if (__atomic_load_n(&timer->state, __ATOMIC_RELAXED) ==
	    kTimerExecuting) {
		ke_spinlock_release_nospl(&timer->hdr.spinlock);
#ifdef __amd64__
		asm("pause");
#endif
		ke_spinlock_acquire_nospl(&timer->hdr.spinlock);
		goto retry;
	} else if (__atomic_load_n(&timer->state, __ATOMIC_RELAXED) ==
	    kTimerInQueue) {
		if (!ki_timer_dequeue_locked(timer))
			goto retry;
	}

	kassert(
	    __atomic_load_n(&timer->state, __ATOMIC_RELAXED) == kTimerDisabled);
	timer->cpu = curcpu();
	timer->deadline = curcpu()->nanos + nanosecs;
	ki_timer_enqueue_locked(timer);

	ke_spinlock_release(&timer->hdr.spinlock, ipl);
}

void
ke_timer_cancel(ktimer_t *timer)
{
	ipl_t ipl = ke_spinlock_acquire(&timer->hdr.spinlock);

retry:
	if (__atomic_load_n(&timer->state, __ATOMIC_RELAXED) ==
	    kTimerExecuting) {
		asm("nop");
		goto retry;
	} else if (__atomic_load_n(&timer->state, __ATOMIC_RELAXED) ==
	    kTimerInQueue) {
		if (!ki_timer_dequeue_locked(timer))
			goto retry;
	}
	kassert(
	    __atomic_load_n(&timer->state, __ATOMIC_RELAXED) == kTimerDisabled);

	ke_spinlock_release(&timer->hdr.spinlock, ipl);
}
