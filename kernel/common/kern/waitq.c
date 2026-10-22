/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file waitq.c
 * @brief Wait queues with chained stack wait blocks.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/k_waitq.h>

void
ke_waitq_init(kwaitq_t *wq)
{
	ke_spinlock_init(&wq->lock);
	TAILQ_INIT(&wq->waiters);
}

void
ke_waitq_prepare(kwaitq_t *wq, kwaiter_t *waiter)
{
	ipl_t ipl;

	ke_event_init(&waiter->event, false);

	ipl = ke_spinlock_enter(&wq->lock);
	TAILQ_INSERT_TAIL(&wq->waiters, waiter, link);
	waiter->queued = true;
	ke_spinlock_exit(&wq->lock, ipl);
}

int
ke_waitq_wait(kwaitq_t *wq, kwaiter_t *waiter, const char *reason,
    bool alertable, kabstime_t deadline)
{
	ipl_t ipl;
	int r;

	r = ke_wait1(&waiter->event, reason, alertable, deadline);
	if (r == 0)
		return 0;

	kassert(alertable || deadline != ABSTIME_FOREVER,
	    "uninterruptible waitq wait failed");

	ipl = ke_spinlock_enter(&wq->lock);
	if (waiter->queued) {
		TAILQ_REMOVE(&wq->waiters, waiter, link);
		waiter->queued = false;
	} else {
		/* intervening wakeup */
		r = 0;
	}
	ke_spinlock_exit(&wq->lock, ipl);

	return r;
}

static void
wake_locked(kwaitq_t *wq, kwaiter_t *waiter)
{
	TAILQ_REMOVE(&wq->waiters, waiter, link);
	waiter->queued = false;
	ke_event_set_signalled(&waiter->event, true);
}

void
ke_waitq_wake_all(kwaitq_t *wq)
{
	kwaiter_t *waiter;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&wq->lock);
	while ((waiter = TAILQ_FIRST(&wq->waiters)) != NULL)
		wake_locked(wq, waiter);
	ke_spinlock_exit(&wq->lock, ipl);
}

void
ke_waitq_quiesce(kwaitq_t *wq)
{
	ipl_t ipl;

	ipl = ke_spinlock_enter(&wq->lock);
	kassert(TAILQ_EMPTY(&wq->waiters), "quiescing a waitq with waiters");
	ke_spinlock_exit(&wq->lock, ipl);
}

bool
ke_waitq_wake_one(kwaitq_t *wq)
{
	kwaiter_t *waiter;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&wq->lock);
	waiter = TAILQ_FIRST(&wq->waiters);
	if (waiter != NULL)
		wake_locked(wq, waiter);
	ke_spinlock_exit(&wq->lock, ipl);

	return waiter != NULL;
}
