/*
 * Copyright (c) 2023-2024 NetaScale Object Solutions.
 * Created on Wed Jan 17 2023.
 */

#include "kdk/libkern.h"
#include "kdk/kern.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "ki.h"

void
ki_wake_waiter(kthread_t *thread)
{
	ipl_t ipl = ke_spinlock_acquire(&thread->lock);
	kassert(thread->state == kThreadStateWaiting &&
	    thread->wait_status == kInternalWaitStatusSatisfied);
	thread->state = kThreadStateRunnable;
	ki_thread_resume_locked(thread);
	ke_spinlock_release(&thread->lock, ipl);
}

void
ki_wake_waiters(kwaitblock_queue_t *queue)
{
	kwaitblock_t *wb;
	TAILQ_FOREACH (wb, queue, queue_entry)
		ki_wake_waiter(wb->thread);
}

void
ki_object_acquire(kdispatchheader_t *hdr, kthread_t *thread)
{
	switch (hdr->type) {
	case kDispatchSemaphore: {
		hdr->signalled--;
		break;
	}

	case kDispatchMutex: {
		kmutex_t *mtx = (kmutex_t *)hdr;
		hdr->signalled--;
		mtx->owner = thread;
		break;
	}

	case kDispatchTimer:
		/* epsilon, timers remain signalled until reset */
		break;

	case kDispatchEvent:
	case kDispatchMsgQueue:
		/* epsilon, msgqueues remain signalled until posted */
		break;

	default:
		kfatal("Invalid kernel object type\n");
	}
}

enum ki_satisfy_attempt_result
ki_waitblock_try_to_satisfy(kwaitblock_t *wb)
{

	kinternalwaitstatus_t expectPreparing, expectWaiting;

	while (true) {
		expectPreparing = kInternalWaitStatusPreparing;
		expectWaiting = kInternalWaitStatusWaiting;

		if (__atomic_compare_exchange_n(wb->waiter_status,
			&expectPreparing, kInternalWaitStatusSatisfied, false,
			__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			/* satisfied a wait pre-wait */
			wb->block_status = kWaitBlockStatusAcquired;
			return kDidSatisfyPreWait;
		} else if (__atomic_compare_exchange_n(wb->waiter_status,
			       &expectWaiting, kInternalWaitStatusSatisfied,
			       false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
			/* satisfied a wait mid-wait */

			/* thread is for waking */
			wb->block_status = kWaitBlockStatusAcquired;
			return kDidSatisfyWait;
		} else if (__atomic_load_n(wb->waiter_status,
			       __ATOMIC_ACQUIRE) ==
		    kInternalWaitStatusSatisfied) {
			/* wait already satisfied by someone else */
			wb->block_status = kWaitBlockStatusDeactivated;
			return kWasAlreadySatisfied;
		}
	}
}

void
ki_signal(kdispatchheader_t *hdr, kwaitblock_queue_t *wakeQueue)
{
	while (!TAILQ_EMPTY(&hdr->waitblock_queue) && hdr->signalled > 0) {
		kwaitblock_t *wb;
		int r;

		wb = TAILQ_FIRST(&hdr->waitblock_queue);
		TAILQ_REMOVE(&hdr->waitblock_queue, wb, queue_entry);

		r = ki_waitblock_try_to_satisfy(wb);
		switch (r) {
		case kDidSatisfyWait:
			TAILQ_INSERT_TAIL(wakeQueue, wb, queue_entry);
			/* fallthrough */

		case kDidSatisfyPreWait:
			ki_object_acquire(hdr, wb->thread);
			break;

		case kWasAlreadySatisfied:
			break;
		}
	}
}

kwaitresult_t
ke_wait(void *object, const char *reason, bool isuserwait, bool alertable,
    nanosecs_t timeout)
{
	return ke_wait_multi(1, &object, reason, false, isuserwait, alertable,
	    timeout, NULL);
}

#define OBJECT(I__) ((I__ == orignobjects) ? &timer : objects[i])
#define WAIT_BLOCK(I__) ((I__ == orignobjects) ? &timer_wb : &waitblocks[i])

kwaitresult_t
ke_wait_multi(size_t nobjects, void *objects[], const char *reason,
    bool isWaitall, bool isUserwait, bool isAlertable, nanosecs_t timeout,
    kwaitblock_t *waitblocks)
{
	ipl_t ipl = spldpc();
	kthread_t *thread = curthread();
	kinternalwaitstatus_t *status = &thread->wait_status;
	int satisfier = -1;
	size_t orignobjects = nobjects;
	ktimer_t timer;
	kwaitblock_t timer_wb;

	kassert(!isWaitall);

	if (timeout != 0 && timeout != -1) {
		ke_timer_init(&timer);
		nobjects++;
	}

	if (waitblocks == NULL) {
		kassert(nobjects <= 4);
		waitblocks = thread->integral_waitblocks;
	}

	__atomic_store_n(status, kInternalWaitStatusPreparing,
	    __ATOMIC_RELEASE);

	for (unsigned i = 0; i < nobjects; i++) {
		kwaitblock_t *wb = WAIT_BLOCK(i);
		kdispatchheader_t *obj = OBJECT(i);

		ke_spinlock_acquire_nospl(&obj->spinlock);

		if (obj->signalled > 0) {
			kinternalwaitstatus_t expected =
			    kInternalWaitStatusPreparing;
			if (__atomic_compare_exchange_n(status, &expected,
				kInternalWaitStatusSatisfied, false,
				__ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				satisfier = i;
				ki_object_acquire(obj, thread);
				ke_spinlock_release_nospl(&obj->spinlock);
				break;
			} else {
				kassert(
				    __atomic_load_n(status, __ATOMIC_ACQUIRE) ==
				    kInternalWaitStatusSatisfied);
				ke_spinlock_release_nospl(&obj->spinlock);
				break;
			}
		}

		wb->waiter_status = &thread->wait_status;
		wb->thread = thread;
		wb->block_status = kWaitBlockStatusActive;

		TAILQ_INSERT_TAIL(&obj->waitblock_queue, wb, queue_entry);

		ke_spinlock_release_nospl(&obj->spinlock);
	}

	if (satisfier != -1 || timeout == 0) {
		int limit = satisfier == -1 ? nobjects : satisfier;
		for (int i = 0; i < limit; i++) {
			kwaitblock_t *wb;
			kdispatchheader_t *obj;

			/*
			 * xxx this looks like a problem
			 * (if there is a timeout it looks like the timer is
			 *  never cancelled, nor removed from the waitblocks!)
			 */

			wb = &waitblocks[i];
			obj = objects[i];

			ke_spinlock_acquire_nospl(&obj->spinlock);
			wb->block_status = kWaitBlockStatusDeactivated;
			TAILQ_REMOVE(&obj->waitblock_queue, wb, queue_entry);
			ke_spinlock_release_nospl(&obj->spinlock);
		}

		splx(ipl);
		return satisfier == -1 ? kKernWaitStatusTimedOut : satisfier;
	}

	if (timeout != 0 && timeout != -1)
		ke_timer_set(&timer, timeout);

	ke_spinlock_acquire_nospl(&thread->lock);
	kinternalwaitstatus_t expected = kInternalWaitStatusPreparing;
	if (__atomic_compare_exchange_n(status, &expected,
		kInternalWaitStatusWaiting, false, __ATOMIC_ACQ_REL,
		__ATOMIC_ACQUIRE)) {
		kassert(ipl < kIPLDPC);
		thread->wait_reason = reason;
		thread->state = kThreadStateWaiting;
		if (thread->port != NULL) {
			kwaitblock_queue_t wb_queue;
			TAILQ_INIT(&wb_queue);
			ki_port_thread_release(thread->port, &wb_queue);
			ki_wake_waiters(&wb_queue);
		}
		ki_reschedule();
	} else {
		/* wait was terminated early. check what happened */
		ke_spinlock_release_nospl(&thread->lock);
	}

	/* at this point, we either slept or wait was early-terminated */

	thread->wait_reason = NULL;

	if (timeout != 0 && timeout != -1)
		ke_timer_cancel(&timer);

	kassert(__atomic_load_n(status, __ATOMIC_ACQUIRE) ==
	    kInternalWaitStatusSatisfied);

	for (unsigned i = 0; i < nobjects; i++) {
		kwaitblock_t *wb = WAIT_BLOCK(i);
		kdispatchheader_t *obj = OBJECT(i);

		ke_spinlock_acquire_nospl(&obj->spinlock);

		switch (wb->block_status) {
		case kWaitBlockStatusActive:
			TAILQ_REMOVE(&obj->waitblock_queue, wb, queue_entry);
			break;

		case kWaitBlockStatusAcquired:
			kassert(satisfier == -1);
			satisfier = i;
			/* fallthrough */
		case kWaitBlockStatusDeactivated:
			break;
		}

		ke_spinlock_release_nospl(&obj->spinlock);
	}

	kassert(satisfier != -1);
	splx(ipl);

	if (timeout != 0 && timeout != -1 && satisfier == orignobjects)
		return kKernWaitStatusTimedOut;
	else
		return satisfier;
}

void
ke_sleep(nanosecs_t nanosecs)
{
	ktimer_t timer;
	ke_timer_init(&timer);
	ke_timer_set(&timer, nanosecs);
	ke_wait(&timer, "sleep", false, false, -1);
}
