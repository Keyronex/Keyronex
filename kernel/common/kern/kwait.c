/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file kwait.c
 * @brief Kernel waitable objects.
 */

#include <keyronex/kwait.h>
#include <keyronex/ktask.h>
#include <keyronex/dlog.h>

#include <sys/errno.h>

#define OBJECT(I) ((I == obj_n) ? &(timeout.header) : objs[i])
#define WAIT_BLOCK(I) ((I == obj_n) ? &timeout_wb : &waitblocks[i])

static void
obj_acquire(struct kdispatch_header *obj, kthread_t *thread)
{
	switch (obj->type) {
	case SYNCH_SEMAPHORE:
		obj->signalled--;
		break;

	case SYNCH_EVENT:
	case SYNCH_CALLOUT:
		/* epsilon */
		break;
	}
}

int
ke_waitn(size_t obj_n, void **objs, const char *reason, bool signallable,
    kabstime_t deadline)
{
	ipl_t ipl;
	kthread_t *thread;
	size_t satisfier = -1;
	kwait_internal_status_t *stat;
	struct kwaitblock *waitblocks;
	size_t real_obj_n = obj_n;
	kcallout_t timeout;
	struct kwaitblock timeout_wb;

	kassert(obj_n < 4, "too many objects");

	if (deadline != ABSTIME_NEVER && deadline != ABSTIME_FOREVER) {
		ke_callout_init(&timeout);
		real_obj_n++;
	}

	thread = ke_curthread();
	stat = &thread->wait_status;
	waitblocks = thread->integral_waitblocks;

	atomic_store_explicit(stat, SYNCH_PRE_WAIT, memory_order_relaxed);

	ipl = spldisp();

	for (unsigned i = 0; i < real_obj_n; i++) {
		struct kwaitblock *wb = WAIT_BLOCK(i);
		struct kdispatch_header *obj = OBJECT(i);

		ke_spinlock_enter_nospl(&obj->lock);

		if (obj->signalled > 0) {
			enum kwait_internal_status expected = SYNCH_PRE_WAIT;
			if (atomic_compare_exchange_strong_explicit(stat,
			    &expected, SYNCH_POST_WAIT, memory_order_acq_rel,
			    memory_order_acquire)) {
				satisfier = i;
				obj_acquire(obj, thread);
				ke_spinlock_exit_nospl(&obj->lock);
				break;
			} else {
				kassert(atomic_load_explicit(stat,
				    memory_order_relaxed) == SYNCH_POST_WAIT,
				    "unexpected wait state");
				ke_spinlock_exit_nospl(&obj->lock);
				break;
			}
		}

		wb->waiter = thread;
		wb->status = WAITBLOCK_ACTIVE;

		TAILQ_INSERT_TAIL(&obj->waitq, wb, qlink);

		ke_spinlock_exit_nospl(&obj->lock);
	}

	if (satisfier != -1 || deadline == ABSTIME_NEVER) {
		/* satisfied early, or this is a poll. */

		size_t limit = satisfier == -1 ? obj_n : satisfier;

		for (int i = 0; i < limit; i++) {
			struct kwaitblock *wb;
			struct kdispatch_header *obj;

			wb = &waitblocks[i];
			obj = objs[i];

			ke_spinlock_enter_nospl(&obj->lock);
			/* FIXME: it could be acquired by now, handle this! */
			kassert(wb->status == WAITBLOCK_ACTIVE, "fixme");
			wb->status = WAITBLOCK_INACTIVE;
			TAILQ_REMOVE(&obj->waitq, wb, qlink);
			ke_spinlock_exit_nospl(&obj->lock);
		}

		splx(ipl);

		return satisfier == -1 ? -ETIMEDOUT : satisfier;
	}

	if (deadline != ABSTIME_NEVER && deadline != ABSTIME_FOREVER)
		ke_callout_set(&timeout, deadline);

	/* commit wait */
	{
		enum kwait_internal_status expected = SYNCH_PRE_WAIT;

		ke_spinlock_enter_nospl(&thread->lock);

		if (atomic_compare_exchange_strong_explicit(stat, &expected,
		    SYNCH_WAIT, memory_order_acq_rel, memory_order_acquire)) {
			kassert(ipl < IPL_DISP, "ipl high in ke_wait");
			thread->wait_reason = reason;
			thread->state = TS_SLEEPING;
			ke_dispatch();
			/* we return here with thread lock released */
		} else {
			/* early wait termination! */
			ke_spinlock_exit_nospl(&thread->lock);
		}
	}

	thread->wait_reason = NULL;
	if (deadline != ABSTIME_NEVER && deadline != ABSTIME_FOREVER)
		ke_callout_stop(&timeout);

	kassert(atomic_load_explicit(stat, memory_order_acquire) ==
	    SYNCH_POST_WAIT, "woke without satisfaction");

	/* unregister wait blocks */
	for (unsigned i = 0; i < real_obj_n; i++) {
		struct kwaitblock *wb = WAIT_BLOCK(i);
		struct kdispatch_header *obj = OBJECT(i);

		ke_spinlock_enter_nospl(&obj->lock);

		switch (wb->status) {
		case WAITBLOCK_ACTIVE:
			TAILQ_REMOVE(&obj->waitq, wb, qlink);
			break;

		case WAITBLOCK_ACQUIRED:
			kassert(satisfier == -1, "multiple satisfiers");
			satisfier = i;
			/* fallthrough */
		case WAITBLOCK_INACTIVE:
			break;
		}

		ke_spinlock_exit_nospl(&obj->lock);
	}

	kassert(satisfier != -1, "expected a satisfier");
	splx(ipl);

	if (satisfier == obj_n)
		return -ETIMEDOUT;
	else
		return satisfier;
}

int
ke_wait1(void *obj, const char *reason, bool signallable, kabstime_t deadline)
{
	return ke_waitn(1, &obj, reason, signallable, deadline);
}

enum {
	SATISFIED_PRE_WAIT,
	SATISFIED_WAIT,
	ALREADY_SATISFIED
} ki_waitblock_try_to_satisfy(struct kwaitblock *wb)
{
	enum kwait_internal_status expect_pre, expect_wait;
	kwait_internal_status_t *stat = &wb->waiter->wait_status;

	while (true) {
		expect_pre = SYNCH_PRE_WAIT;
		expect_wait = SYNCH_WAIT;

		if (atomic_compare_exchange_strong_explicit(stat, &expect_pre,
			SYNCH_POST_WAIT, memory_order_release,
			memory_order_relaxed)) {
			wb->status = WAITBLOCK_ACQUIRED;
			return SATISFIED_PRE_WAIT;
		} else if (atomic_compare_exchange_strong_explicit(stat,
			    &expect_wait, SYNCH_POST_WAIT, memory_order_release,
			    memory_order_relaxed)) {
			wb->status = WAITBLOCK_ACQUIRED;
			return SATISFIED_WAIT;
		} else if (atomic_load_explicit(stat, memory_order_relaxed) ==
		    SYNCH_POST_WAIT) {
			wb->status = WAITBLOCK_INACTIVE;
			return ALREADY_SATISFIED;
		}
	}
}

void
kep_signal(struct kdispatch_header *obj, struct kwaitblock_queue *wakeq)
{
	while (!TAILQ_EMPTY(&obj->waitq) && obj->signalled > 0) {
		struct kwaitblock *wb;
		int r;

		wb = TAILQ_FIRST(&obj->waitq);
		TAILQ_REMOVE(&obj->waitq, wb, qlink);

		r = ki_waitblock_try_to_satisfy(wb);
		switch (r) {
		case SATISFIED_WAIT:
			TAILQ_INSERT_TAIL(wakeq, wb, qlink);
			/* fallthrough */

		case SATISFIED_PRE_WAIT:
			obj_acquire(obj, wb->waiter);
			break;

		case ALREADY_SATISFIED:
			break;
		}
	}
}

void
kep_waiters_wake(struct kwaitblock_queue *wakeq)
{
	struct kwaitblock *wb, *next;

	TAILQ_FOREACH_SAFE(wb, wakeq, qlink, next) {
		TAILQ_REMOVE(wakeq, wb, qlink);
		ke_thread_resume(wb->waiter, false);
	}
}

void
kep_dispatcher_obj_init(struct kdispatch_header *obj, int signal,
    enum synch_type type)
{
	ke_spinlock_init(&obj->lock);
	obj->signalled = signal;
	obj->type = type;
	TAILQ_INIT(&obj->waitq);
}
