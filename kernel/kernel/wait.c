/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Jan 17 2023.
 */

#include "kdk/libkern.h"
#include "kdk/vm.h"
#include "kernel/ke_internal.h"

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

void
ki_object_acquire(kthread_t *thread, kdispatchheader_t *hdr)
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

static void
wait_timeout_callback(void *arg)
{
	kthread_t *thread = arg;
	ipl_t ipl = ke_acquire_dispatcher_lock();

	kassert(thread->state == kThreadStateWaiting &&
	    thread->wait_result == kKernWaitStatusWaiting);

	for (unsigned i = 0; i < thread->nwaits; i++) {
		TAILQ_REMOVE(&thread->waitblocks[i].object->waitblock_queue,
		    &thread->waitblocks[i], queue_entry);
	}

	waiter_wake(thread, kKernWaitStatusTimedOut);

	ke_release_dispatcher_lock(ipl);
}

/*! dispatcher lock held */
bool
ki_waiter_maybe_wakeup(kthread_t *thread, kdispatchheader_t *hdr)
{
	kassert((uintptr_t)thread >= KAREA_BASE);
	if (thread->iswaitall) {
		bool acquirable = true;

		for (unsigned i = 0; i < thread->nwaits; i++) {
			if (thread->waitblocks[i].object->signalled <= 0)
				acquirable = false;
		}

		if (acquirable) {
			/* all acquirable, so remove thread from all
			 * waitblock queues and acquire each waited
			 * object. and dequeue any wait. */
			if (thread->wait_timer.state == kTimerPending)
				ki_timer_dequeue(&thread->wait_timer);
			for (unsigned i = 0; i < thread->nwaits; i++) {
				TAILQ_REMOVE(&thread->waitblocks[i]
						  .object->waitblock_queue,
				    &thread->waitblocks[i], queue_entry);
				ki_object_acquire(thread,
				    thread->waitblocks[i].object);
			}
			waiter_wake(thread, kKernWaitStatusOK);
			return true;
		} else {
			return false;
		}
	} else {
		/* waiting for any, so remove thread from all waitblock
		 * queues
		 */
		if (thread->wait_timer.state == kTimerPending)
			ki_timer_dequeue(&thread->wait_timer);

		for (unsigned i = 0; i < thread->nwaits; i++) {
			TAILQ_REMOVE(
			    &thread->waitblocks[i].object->waitblock_queue,
			    &thread->waitblocks[i], queue_entry);
		}
		ki_object_acquire(thread, hdr);
		waiter_wake(thread, kKernWaitStatusOK);
		return true;
	}
}

kwaitstatus_t
ke_wait(void *object, const char *reason, bool isuserwait, bool alertable,
    nanosecs_t timeout)
{
	return ke_wait_multi(1, &object, reason, true, isuserwait, alertable,
	    timeout, NULL);
}

kwaitstatus_t
ke_wait_multi(size_t nobjects, void *objects[], const char *reason,
    bool isWaitall, bool isUserwait, bool isAlertable, nanosecs_t timeout,
    krx_nullable krx_out kwaitblock_t *waitblocks)
{
	kthread_t *thread = ke_curthread();
	ipl_t ipl;
	bool satisfied = true;

	if (nobjects > kNThreadWaitBlocks && waitblocks == NULL)
		return kKernWaitStatusInvalidArgument;

	if (waitblocks == NULL)
		waitblocks = thread->integral_waitblocks;

	memset(waitblocks, 0, sizeof(kwaitblock_t) * nobjects);

	ipl = ke_acquire_dispatcher_lock();

	/*
	 * do an initial loop of the objects to determine whether any
	 * are signalled, and if so and we are not isWaitall, then
	 * acquire and break.
	 *
	 * we do not enqueue the waitblock on the object's queue yet,
	 * since we may break early, or only be polling (timeout=0). we
	 * do set the other fields of the waitblock appropriately
	 * because why not.
	 */
	for (int i = 0; i < nobjects; i++) {
		kwaitblock_t *wb = &waitblocks[i];
		kdispatchheader_t *hdr = objects[i];

		wb->object = hdr;
		wb->thread = thread;

		if (hdr->signalled > 0 && !isWaitall) {
			satisfied = true;
			wb->acquired = true;
			ki_object_acquire(thread, hdr);
			break;
		} else if (!hdr->signalled) {
			satisfied = false;
		}
	}

	if (satisfied && !isWaitall) {
		/* we have acquired at least one */
		ke_release_dispatcher_lock(ipl);
		return kKernWaitStatusOK;
	} else if (satisfied && isWaitall) {
		/* all of them are acquirable, so acquire them all */
		for (int i = 0; i < nobjects; i++) {
			kdispatchheader_t *hdr = objects[i];
			ki_object_acquire(thread, hdr);
		}
		ke_release_dispatcher_lock(ipl);
		return kKernWaitStatusOK;

	} else if (timeout == 0) {
		/* only a poll */
		ke_release_dispatcher_lock(ipl);
		return kKernWaitStatusTimedOut;
	}

	for (int i = 0; i < nobjects; i++) {
		kwaitblock_t *wb = &waitblocks[i];
		kdispatchheader_t *hdr = objects[i];

		TAILQ_INSERT_TAIL(&hdr->waitblock_queue, wb, queue_entry);
	}

	thread->state = kThreadStateWaiting;
	thread->wait_reason = reason;
	thread->wait_result = kKernWaitStatusWaiting;
	thread->nwaits = nobjects;
	thread->iswaitall = isWaitall;
	thread->waitblocks = waitblocks;
	thread->saved_ipl = ipl;
	/* enqueue the timeout callout if needed */
	if (timeout != -1 && timeout > 1000) {
		thread->wait_timer.dpc.arg = thread;
		thread->wait_timer.dpc.callback = wait_timeout_callback;
		thread->wait_timer.nanosecs = timeout;
		ki_timer_enqueue(&thread->wait_timer);
	}

#if DEBUG_SCHED == 1
	kdprintf("ke_wait_multi: thread %p going to sleep on %s\n", thread,
	    reason);
#endif
	ki_reschedule();

	splx(ipl);

#if DEBUG_SCHED == 1
	kdprintf("ke_wait_multi: thread %p woke on %s\n", thread, reason);
#endif

	return thread->wait_result;
}