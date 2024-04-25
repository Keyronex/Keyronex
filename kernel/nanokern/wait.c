/*
 * Copyright (c) 2023-2024 NetaScale Object Solutions.
 * Created on Wed Jan 17 2023.
 */

#include "kdk/nanokern.h"
#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "ki.h"

void
ki_wake_waiters(kwaitblock_queue_t *queue)
{
	kwaitblock_t *wb;

	TAILQ_FOREACH (wb, queue, queue_entry) {
		kthread_t *thread = wb->thread;

		kassert(thread->state == kThreadStateWaiting);
		thread->state = kThreadStateRunnable;
		ki_thread_resume_locked(thread);
	}
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
	case kDispatchTimer: {
		/* epsilon, timers remain signalled until reset */
	}

	case kDispatchEvent:
	case kDispatchMsgQueue: {
		/* epsilon, msgqueues remain signalled until posted */
	}
	}
}

void
ki_signal(kdispatchheader_t *hdr, kwaitblock_queue_t *wakeQueue)
{
	while (!TAILQ_EMPTY(&hdr->waitblock_queue) && hdr->signalled > 0) {
		kinternalwaitstatus_t expectPreparing, expectWaiting;
		kthread_t *thread;
		kwaitblock_t *wb;

		wb = TAILQ_FIRST(&hdr->waitblock_queue);
		TAILQ_REMOVE(&hdr->waitblock_queue, wb, queue_entry);
		thread = wb->thread;

		while (true) {
			expectPreparing = kInternalWaitStatusPreparing;
			expectWaiting = kInternalWaitStatusWaiting;

			if (__atomic_compare_exchange_n(wb->waiter_status,
				&expectPreparing, kInternalWaitStatusSatisfied,
				false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				/* satisfied a wait pre-wait */

				wb->block_status = kWaitBlockStatusAcquired;
				// if (!wb->isWaitAll)
				ki_object_acquire(hdr, thread);
				break;
			} else if (__atomic_compare_exchange_n(
				       wb->waiter_status, &expectWaiting,
				       kInternalWaitStatusSatisfied, false,
				       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
				/* satisfied a wait mid-wait */

				/* thread is for waking */
				TAILQ_INSERT_TAIL(wakeQueue, wb, queue_entry);
				wb->block_status = kWaitBlockStatusAcquired;
				ki_object_acquire(hdr, thread);
				break;
			} else if (__atomic_load_n(wb->waiter_status,
				       __ATOMIC_ACQUIRE) ==
			    kInternalWaitStatusSatisfied) {
				/* wait already satisfied by someone else */
				wb->block_status = kWaitBlockStatusDeactivated;
				break;
			}
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

		wb->object = obj;
		wb->waiter_status = &thread->wait_status;
		wb->thread = thread;
		wb->block_status = kWaitBlockStatusActive;

		TAILQ_INSERT_TAIL(&obj->waitblock_queue, wb, queue_entry);

		ke_spinlock_release_nospl(&obj->spinlock);
	}

	if (satisfier != -1 || timeout == 0) {
		for (int i = 0; i < satisfier; i++) {
			kwaitblock_t *wb;
			kdispatchheader_t *obj;

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

	kinternalwaitstatus_t expected = kInternalWaitStatusPreparing;

	if (timeout != 0 && timeout != -1)
		ke_timer_set(&timer, timeout);

	ke_acquire_scheduler_lock();
	if (__atomic_compare_exchange_n(status, &expected,
		kInternalWaitStatusWaiting, false, __ATOMIC_ACQ_REL,
		__ATOMIC_ACQUIRE)) {
		kassert(ipl < kIPLDPC);
		thread->state = kThreadStateWaiting;
		ki_reschedule();
	} else {
		/* wait was terminated early. check what happened */
		ke_release_scheduler_lock(kIPLDPC);
	}

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
