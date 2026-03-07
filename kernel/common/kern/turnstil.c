/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file turnstil.c
 * @brief Turnstiles
 */

#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/k_intr.h>

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

static bool
boost(kturnstile_t *ts, kthread_t *inheritor, kpri_t donate)
{
	kpri_t old_epri = ke_thread_epri_locked(inheritor);

	/* inheritor and ts chain are locked by caller */

	if (ts->inheritor == NULL) {
		ts->inheritor = inheritor;
		ts->pri = donate;
		SLIST_INSERT_HEAD(&inheritor->pi_head, ts, pi_link);
	} else {
		kassert(ts->inheritor == inheritor);
		if (donate > ts->pri)
			ts->pri = donate;
	}

	if (donate > inheritor->inherited_prio)
		ke_thread_set_ipri_locked(inheritor, donate);

	return ke_thread_epri_locked(inheritor) > old_epri;
}

static void
lend_priority(kthread_t *curthread, struct kturnstile_chain *root_chain)
{
	kpri_t donate;
	struct kturnstile_chain *held_chain = NULL;
	kthread_t *thread = curthread;

	kassert(ke_ipl() == IPL_DISP);

restart:
	/* Only want curthread->lock and root_chain->lock. */
	if (held_chain != NULL) {
		ke_spinlock_exit_nospl(&held_chain->lock);
		held_chain = NULL;
	}
	if (thread != curthread) {
		ke_spinlock_exit_nospl(&thread->lock);
		ke_spinlock_enter_nospl(&curthread->lock);
		thread = curthread;
	}

	donate = ke_thread_epri_locked(curthread);

	for (;;) {
		void *obj = thread->waiting_on;
		struct kturnstile_chain *need;
		kturnstile_t *ts;
		kthread_t *owner;

		if (obj == NULL)
			break;

		need = &ts_chains[TC_HASH(obj)];

		if (need == root_chain) {
			if (held_chain != NULL) {
				ke_spinlock_exit_nospl(&held_chain->lock);
				held_chain = NULL;
			}
		} else if (held_chain != need) {
			if (held_chain != NULL) {
				ke_spinlock_exit_nospl(&held_chain->lock);
				held_chain = NULL;
			}
			if (!ke_spinlock_tryenter_nospl(&need->lock))
				goto restart;
			held_chain = need;
		}

		if (thread->waiting_on != obj)
			goto restart;

		ts = thread->turnstile;
		owner = ts->owner;
		if (owner == NULL)
			break;

		if (owner == curthread)
			kfatal("deadlock detected: cycle in blocking chain");

		/*
		 * non-trylock should be fine since the thread->lock then
		 * chain->lock case has to use tryenter on the chain.
		 */
		ke_spinlock_enter_nospl(&owner->lock);

		if (!boost(ts, owner, donate)) {
			/* owner already >= donate, no further PI to do. */
			ke_spinlock_exit_nospl(&owner->lock);
			break;
		}

		ke_spinlock_exit_nospl(&thread->lock);

		if (held_chain != NULL) {
			ke_spinlock_exit_nospl(&held_chain->lock);
			held_chain = NULL;
		}

		thread = owner; /* owner->lock still held */
	}

	if (held_chain != NULL)
		ke_spinlock_exit_nospl(&held_chain->lock);

	if (thread != curthread) {
		ke_spinlock_exit_nospl(&thread->lock);
		ke_spinlock_enter_nospl(&curthread->lock);
	}
}

static void
revoke_priority(kturnstile_t *ts)
{
	kthread_t *inheritor = ts->inheritor;
	kpri_t new_ipri;
	kturnstile_t *it;

	kassert(ke_ipl() == IPL_DISP);

	kassert(inheritor != NULL);
	ke_spinlock_enter_nospl(&inheritor->lock);

	SLIST_REMOVE(&inheritor->pi_head, ts, kturnstile, pi_link);

	ts->inheritor = NULL;
	ts->pri = 0;

	new_ipri = 0;
	SLIST_FOREACH(it, &inheritor->pi_head, pi_link)
		if (it->pri > new_ipri)
			new_ipri = it->pri;
	ke_thread_set_ipri_locked(inheritor, new_ipri);

	ke_spinlock_exit_nospl(&inheritor->lock);
}

void
ke_turnstile_block(kturnstile_t *ts, bool writer, void *obj, kthread_t *owner,
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
		ts->owner = owner;
		LIST_INSERT_HEAD(&chain->list, ts, hash_link);
	} else {
		kassert(ts->owner == owner);
		SLIST_INSERT_HEAD(&ts->freelist, thread->turnstile, free_link);
		thread->turnstile = ts;
	}

	ke_event_init(&wb.event, false);
	wb.thread = thread;
	TAILQ_INSERT_TAIL(&ts->waiters[writer], &wb, qentry);
	ts->nwaiters[writer]++;

	thread->waiting_on = obj;
	ke_spinlock_enter_nospl(&thread->lock);
	lend_priority(thread, chain);
	ke_spinlock_exit_nospl(&chain->lock);
	ke_spinlock_exit_nospl(&thread->lock);
	splx(ipl);
	ke_wait1(&wb.event, "turnstile_block", false, ABSTIME_FOREVER);
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

	ke_event_set_signalled(&wb->event, true);
}

/* todo: a heap instead maybe? */
static kpri_t
max_waiter_pri(kturnstile_t *ts)
{
	kpri_t max = 0;
	for (size_t q = 0; q < 2; q++) {
		struct kturnstile_waiter *wb;
		TAILQ_FOREACH(wb, &ts->waiters[q], qentry) {
			kpri_t epri;
			ke_spinlock_enter_nospl(&wb->thread->lock);
			epri = ke_thread_epri_locked(wb->thread);
			ke_spinlock_exit_nospl(&wb->thread->lock);
			if (epri > max)
				max = epri;
		}
	}
	return max;
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

		ts->owner = newowner;

		if ((ts->nwaiters[0] + ts->nwaiters[1]) > 0) {
			kpri_t donate = max_waiter_pri(ts);
			if (donate > 0) {
				ke_spinlock_enter_nospl(&newowner->lock);
				boost(ts, newowner, donate);
				ke_spinlock_exit_nospl(&newowner->lock);
			}
		}
	} else {
		while (count-- > 0) {
			wb = TAILQ_FIRST(&ts->waiters[writer]);
			kassert(wb != NULL);
			wake_waiter(ts, writer, wb);
		}

		ts->owner = NULL;
	}

	ke_spinlock_exit(&chain->lock, ipl);
}

void
ke_turnstile_exit(void *obj, ipl_t ipl)
{
	struct kturnstile_chain *chain = &ts_chains[TC_HASH(obj)];
	ke_spinlock_exit(&chain->lock, ipl);
}
