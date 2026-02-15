/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file turnstil.c
 * @brief Turnstiles
 */

#include <keyronex/dlog.h>
#include <keyronex/ktask.h>
#include <keyronex/kwait.h>
#include <keyronex/intr.h>

#include <libkern/queue.h>

struct kturnstile_waiter {
	TAILQ_ENTRY(kturnstile_waiter) qentry;
	kthread_t *thread;
	kevent_t event;
};

struct kturnstile_chain {
	LIST_HEAD(, kturnstile)	list;
	kspinlock_t lock;
};

#define TC_COUNT 128
#define TC_MASK (TC_COUNT - 1)
#define TC_SHIFT 6
#define TC_HASH(PTR) (((uintptr_t)(PTR) >> TC_SHIFT) & TC_MASK)
static struct kturnstile_chain ts_chains[TC_COUNT];

void
kep_turnstile_init(void)
{
	for (size_t i = 0; i < TC_COUNT; i++) {
		LIST_INIT(&ts_chains[i].list);
		ke_spinlock_init(&ts_chains[i].lock);
	}
}

ipl_t
ke_turnstile_lookup(void *obj, kturnstile_t **out)
{
	kturnstile_t *ts;
	ipl_t ipl;
	struct kturnstile_chain *chain = &ts_chains[TC_HASH(obj)];

	ipl = ke_spinlock_enter(&chain->lock);
	LIST_FOREACH(ts, &chain->list, hash_link)
		if (ts->obj == obj)
			break;

	*out = ts;

	return ipl;
}


kthread_t *
ke_turnstile_waiter(kturnstile_t *ts, bool writer)
{
	kassert(!TAILQ_EMPTY(&ts->waiters[writer]));
	return TAILQ_FIRST(&ts->waiters[writer])->thread;
}

static void
lend_priority(kthread_t *thread)
{
	/* todo */
}

void
ke_turnstile_block(kturnstile_t *ts, bool writer, void *obj, ksyncops_t *ops,
	ipl_t ipl)
{
	kthread_t *thread = ke_curthread();
	struct kturnstile_waiter wb;
	struct kturnstile_chain *chain = &ts_chains[TC_HASH(obj)];

	if (ts == NULL) {
		ts = thread->turnstile;
		SLIST_INIT(&ts->freelist);
		for (size_t i = 0; i < 2; i++) {
			TAILQ_INIT(&ts->waiters[i]);
			ts->nwaiters[i] = 0;
		}
		ts->obj = obj;
		ts->inheritor = NULL;
		LIST_INSERT_HEAD(&chain->list, ts, hash_link);
	} else {
		SLIST_INSERT_HEAD(&ts->freelist, thread->turnstile, free_link);
		thread->turnstile = ts;
	}

	ke_event_init(&wb.event, false);
	wb.thread = thread;
	TAILQ_INSERT_TAIL(&ts->waiters[writer], &wb, qentry);
	ts->nwaiters[writer]++;

	thread->waiting_on = obj;
	thread->sync_ops = ops;

	ke_spinlock_enter_nospl(&thread->lock);
	lend_priority(thread);
	ke_spinlock_exit_nospl(&chain->lock);
	ke_dispatch();
	splx(ipl);
}

static void
revoke_priority(kturnstile_t *ts)
{
	/* todo */
}

static void
wake_waiter(kturnstile_t *ts, bool writer, struct kturnstile_waiter *wb)
{
	kthread_t *thread = wb->thread;

	/*
	 * If there are turnstiles on the freelist, then steal one for the
	 * waiter thread. Otherwise, this must be the last waiter, so it can
	 * keep the turnstile.
	 */
	if (!SLIST_EMPTY(&ts->freelist)) {
		kturnstile_t *newts = SLIST_FIRST(&ts->freelist);

		kassert((ts->nwaiters[0] + ts->nwaiters[1]) > 1);

		SLIST_REMOVE_HEAD(&ts->freelist, free_link);
		wb->thread->turnstile = newts;
	} else {
		kassert((ts->nwaiters[0] + ts->nwaiters[1]) == 1);
		LIST_REMOVE(ts, hash_link);
		ts->obj = NULL;
		ts->inheritor = NULL;
	}

	TAILQ_REMOVE(&ts->waiters[writer], wb, qentry);
	ts->nwaiters[writer]--;

	thread->waiting_on = NULL;
	thread->sync_ops = NULL;

	ke_event_set_signalled(&wb->event, true);
}

void
ke_turnstile_wakeup(kturnstile_t *ts, bool writer, int count,
    kthread_t *newowner, ipl_t ipl)
{
	struct kturnstile_waiter *wb;
	struct kturnstile_chain *chain = &ts_chains[TC_HASH(ts->obj)];

	if (ts->inheritor != NULL)
		revoke_priority(ts);

	if (newowner != NULL) {
		TAILQ_FOREACH( wb, &ts->waiters[writer], qentry)
			if (wb->thread == newowner)
				break;

		if (wb == NULL)
			kfatal("no such waiter on turnstile");

		wake_waiter(ts, writer, wb);
	} else {
		while (count-- > 0) {
			wb = TAILQ_FIRST(&ts->waiters[writer]);
			kassert(wb != NULL);
			wake_waiter(ts, writer, wb);
		}
	}

	ke_spinlock_exit(&chain->lock, ipl);
}

void
ke_turnstile_exit(void *obj, ipl_t ipl)
{
	struct kturnstile_chain *chain = &ts_chains[TC_HASH(obj)];
	ke_spinlock_exit(&chain->lock, ipl);
}
