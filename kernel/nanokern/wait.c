#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>
#include <string.h>

#include "md/spl.h"

void
nkx_object_acquire(kthread_t *thread, kdispatchheader_t *hdr)
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
	}
}

/*! ipl = dispatch, nk_lock held */
static void
waiter_wake(kthread_t *thread, kwaitstatus_t result)
{
	thread->wait_result = result;
	ipl_t ipl = nk_spinlock_acquire_at(&thread->cpu->sched_lock, kSPLHigh);
	TAILQ_INSERT_HEAD(&thread->cpu->runqueue, thread, queue_link);
	if (thread->cpu == curcpu()) {
		nk_raise_dispatch_interrupt();
	} else {
		md_ipi_reschedule(thread->cpu);
	}
	nk_spinlock_release(&thread->cpu->sched_lock, ipl);
}

static void
wait_timeout_callback(void *arg)
{
	kthread_t *thread = arg;
	ipl_t	   ipl = nk_spinlock_acquire(&nk_lock);

	nk_assert(thread->state == kThreadStateWaiting &&
	    thread->wait_result == kKernWaitStatusWaiting);

	for (unsigned i = 0; i < thread->nwaits; i++) {
		TAILQ_REMOVE(&thread->waitblocks[i].object->waitblock_queue,
		    &thread->waitblocks[i], queue_entry);
	}

	waiter_wake(thread, kKernWaitStatusTimedOut);

	nk_spinlock_release(&nk_lock, ipl);
}

/*! ipl=dispatch, nk_lock held */
bool
nkx_waiter_maybe_wakeup(kthread_t *thread, kdispatchheader_t *hdr)
{
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
			nkx_callout_dequeue(&thread->wait_callout);
			for (unsigned i = 0; i < thread->nwaits; i++) {
				TAILQ_REMOVE(&thread->waitblocks[i]
						  .object->waitblock_queue,
				    &thread->waitblocks[i], queue_entry);
				nkx_object_acquire(thread,
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
		for (unsigned i = 0; i < thread->nwaits; i++) {
			TAILQ_REMOVE(&thread->waitblocks[i]
					  .object->waitblock_queue,
			    &thread->waitblocks[i], queue_entry);
		}

		nkx_object_acquire(thread, hdr);
		nkx_callout_dequeue(&thread->wait_callout);
		waiter_wake(thread, kKernWaitStatusOK);
		return true;
	}
}

kwaitstatus_t
nk_wait(void *object, const char *reason, bool isuserwait, bool alertable,
    nanosec_t timeout)
{
	return nk_wait_multi(1, &object, reason, true, isuserwait, alertable,
	    timeout, NULL);
}

kwaitstatus_t
nk_wait_multi(size_t nobjects, void *objects[], const char *reason,
    bool isWaitall, bool isUserwait, bool isAlertable, nanosec_t timeout,
    kx_nullable kx_out kwaitblock_t *waitblocks)
{
	kthread_t *thread = curcpu()->running_thread;
	ipl_t	   ipl = spldispatch();
	bool	   satisfied = true;

	nk_assert(ipl <= kSPLDispatch);

	if (nobjects > kNThreadWaitBlocks && waitblocks == NULL)
		return kKernWaitStatusInvalidArgument;

	if (!waitblocks)
		waitblocks = thread->integral_waitblocks;

	memset(waitblocks, 0, sizeof(kwaitblock_t) * nobjects);

	nk_spinlock_acquire(&nk_lock);

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
		kwaitblock_t	  *wb = &waitblocks[i];
		kdispatchheader_t *hdr = objects[i];

		wb->object = hdr;
		wb->thread = thread;

		if (hdr->signalled > 0 && !isWaitall) {
			satisfied = true;
			wb->acquired = true;
			nkx_object_acquire(thread, hdr);
			break;
		} else if (!hdr->signalled) {
			satisfied = false;
		}
	}

	if (satisfied && !isWaitall) {
		/* we have acquired at least one */
		nk_spinlock_release(&nk_lock, ipl);
		return kKernWaitStatusOK;
	} else if (satisfied && isWaitall) {
		/* all of them are acquirable, so acquire them all */
		for (int i = 0; i < nobjects; i++) {
			kdispatchheader_t *hdr = objects[i];
			nkx_object_acquire(thread, hdr);
		}
		nk_spinlock_release(&nk_lock, ipl);
		return kKernWaitStatusOK;

	} else if (timeout == 0) {
		/* only a poll */
		return kKernWaitStatusTimedOut;
	}

	for (int i = 0; i < nobjects; i++) {
		kwaitblock_t	  *wb = &waitblocks[i];
		kdispatchheader_t *hdr = objects[i];

		TAILQ_INSERT_TAIL(&hdr->waitblock_queue, wb, queue_entry);
	}

	thread->state = kThreadStateWaiting;
	thread->wait_result = kKernWaitStatusWaiting;
	thread->nwaits = nobjects;
	thread->iswaitall = isWaitall;
	thread->waitblocks = waitblocks;
	thread->saved_ipl = ipl;
	/* enqueue the timeout callout if needed */
	if (timeout != -1 && timeout > 1000) {
		nk_dbg("enqueueing a timeout %lu...!\n", timeout);
		thread->wait_callout.dpc.arg = thread;
		thread->wait_callout.dpc.callback = wait_timeout_callback;
		thread->wait_callout.nanosecs = timeout;
		nkx_callout_enqueue(&thread->wait_callout);
	}

	/* should we really be dropping the lock here? */
#if DEBUG_SCHED == 1
	nk_dbg("nk_wait_multi: going to sleep\n");
#endif
	nk_spinlock_release_nospl(&nk_lock);
	nkx_do_reschedule(ipl);

	return thread->wait_result;
}
