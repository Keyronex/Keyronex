#include "kdk/kern.h"
#include "kdk/queue.h"
#include "ki.h"

static struct kport_msg *
do_port_acquire(kport_t *kport, struct kport_msg *msg, kthread_t *thread)
{
	if (msg == NULL) {
		msg = TAILQ_FIRST(&kport->queue);
		TAILQ_REMOVE(&kport->queue, msg, queue_entry);
		kport->hdr.signalled--;
	}

	thread->port = kport;
	thread->port_msg = msg;

	kport->n_processing++;

	return msg;
}

bool
do_release_threads(kport_t *kport, struct kport_msg *msg,
    kwaitblock_queue_t *wb_queue)
{
	bool did_release = false;

	while (!TAILQ_EMPTY(&kport->hdr.waitblock_queue) &&
	    (msg != NULL || !TAILQ_EMPTY(&kport->queue))) {
		kwaitblock_t *wb;
		enum ki_satisfy_attempt_result r;

		if (kport->n_processing >= kport->max_n_processing)
			break;

		wb = TAILQ_LAST(&kport->hdr.waitblock_queue, kwaitblock_queue);
		TAILQ_REMOVE(&kport->hdr.waitblock_queue, wb, queue_entry);

		r = ki_waitblock_try_to_satisfy(wb);
		if (r == kWasAlreadySatisfied)
			continue;

		if (msg == NULL) {
			msg = TAILQ_FIRST(&kport->queue);
			TAILQ_REMOVE(&kport->queue, msg, queue_entry);
		}

		do_port_acquire(kport, msg, wb->thread);

		msg = NULL;
		did_release = true;

		if (r == kDidSatisfyWait)
			TAILQ_INSERT_TAIL(wb_queue, wb, queue_entry);
	}

	return did_release;
}

bool
ki_port_thread_release(kport_t *kport, kwaitblock_queue_t *wb_queue)
{
	bool r;
	ipl_t ipl = ke_spinlock_acquire(&kport->hdr.spinlock);
	r = do_release_threads(kport, NULL, wb_queue);
	ke_spinlock_release(&kport->hdr.spinlock, ipl);
	return r;
}

void
ke_port_enqueue(kport_t *kport, struct kport_msg *msg)
{
	kwaitblock_queue_t wb_queue = TAILQ_HEAD_INITIALIZER(wb_queue);
	ipl_t ipl = ke_spinlock_acquire(&kport->hdr.spinlock);

	if (do_release_threads(kport, msg, &wb_queue)) {
		ke_spinlock_release_nospl(&kport->hdr.spinlock);
		ke_acquire_scheduler_lock();
		ki_wake_waiters(&wb_queue);
		ke_release_scheduler_lock(ipl);
		return;
	}

	kport->hdr.signalled++;
	TAILQ_INSERT_TAIL(&kport->queue, msg, queue_entry);
	ke_spinlock_release(&kport->hdr.spinlock, ipl);
}

struct kport_msg *
ke_port_dequeue(kport_t *kport)
{
	ipl_t ipl = spldpc();
	kthread_t *thread = curthread();
	kinternalwaitstatus_t *status = &thread->wait_status;
	kwaitblock_t *wb = thread->integral_waitblocks;
	kdispatchheader_t *obj = &kport->hdr;
	struct kport_msg *msg = NULL;

	__atomic_store_n(status, kInternalWaitStatusPreparing,
	    __ATOMIC_RELEASE);

	if (thread->port != kport) {
		kwaitblock_queue_t wb_queue = TAILQ_HEAD_INITIALIZER(wb_queue);
		ki_port_thread_release(thread->port, &wb_queue);
		ke_acquire_scheduler_lock();
		ki_wake_waiters(&wb_queue);
		ke_release_scheduler_lock(kIPLDPC);
	}

	ke_spinlock_acquire_nospl(&obj->spinlock);

	if (thread->port == kport) {
		kport->n_processing--;
	}

	if (obj->signalled > 0 &&
	    kport->n_processing < kport->max_n_processing) {
		msg = do_port_acquire(kport, NULL, thread);
	} else {
		wb->object = obj;
		wb->waiter_status = &thread->wait_status;
		wb->thread = thread;
		wb->block_status = kWaitBlockStatusActive;

		TAILQ_INSERT_TAIL(&obj->waitblock_queue, wb, queue_entry);
	}
	ke_spinlock_release_nospl(&obj->spinlock);

	/* a message was already available & n threads was < max threads. */
	if (msg != NULL) {
		splx(ipl);
		thread->port_msg = NULL;
		return msg;
	}

	ke_acquire_scheduler_lock();
	kinternalwaitstatus_t expected = kInternalWaitStatusPreparing;
	if (__atomic_compare_exchange_n(status, &expected,
		kInternalWaitStatusWaiting, false, __ATOMIC_ACQ_REL,
		__ATOMIC_ACQUIRE)) {
		kassert(ipl < kIPLDPC);
		thread->wait_reason = "ke_port_dequeue";
		thread->state = kThreadStateWaiting;
		ki_reschedule();
	} else {
		/* wait was terminated early. check what happened */
		ke_release_scheduler_lock(kIPLDPC);
	}

	/* by this point, wait was early-terminated or we slept. */
	kassert(__atomic_load_n(status, __ATOMIC_ACQUIRE) ==
	    kInternalWaitStatusSatisfied);
	thread->wait_reason = NULL;

	/*
	 * currently, no timeout, AST, or alert support - therefore we know that
	 * we must have been satisfied with a message received.
	 */

	kassert(wb->block_status == kWaitBlockStatusAcquired);

	splx(ipl);

	msg = thread->port_msg;
	thread->port_msg = NULL;

	return msg;
}
