/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file strsched.c
 * @brief STREAMS scheduler.
 */

#include <sys/k_cpu.h>
#include <sys/k_wait.h>
#include <sys/kmem.h>
#include <sys/proc.h>
#include <sys/strsubr.h>

void str_unlock(stdata_t *st);

void
str_kick(stdata_t *st)
{
	struct str_per_cpu_scheduler *sc;
	uint32_t flags;

	atomic_fetch_or(&st->flags, ST_NEEDRUN);

	sc = ke_cpu_data[st->home_cpu]->str_scheduler;

	ke_spinlock_enter_nospl(&sc->lock);

	flags = atomic_load(&st->flags);

	/* not for queuing if already queued, running now, frozen, or dead */
	if (flags & (ST_QUEUED | ST_RUNNING | ST_FROZEN | ST_DEAD)) {
		ke_spinlock_exit_nospl(&sc->lock);
		return;
	}

	atomic_fetch_or(&st->flags, ST_QUEUED);
	TAILQ_INSERT_TAIL(&sc->runq, st, sched_link);
	ke_event_set_signalled(&sc->event, true);

	ke_spinlock_exit_nospl(&sc->lock);
}

static inline void
detach_ingress(stdata_t *st, mblk_q_t *mq)
{
	TAILQ_INIT(mq);
	ke_spinlock_enter_nospl(&st->ingress_lock);
	TAILQ_CONCAT(mq, &st->ingress_head, link);
	ke_spinlock_exit_nospl(&st->ingress_lock);
}

static inline void
drain_ingress(stdata_t *st)
{
	mblk_q_t mq;

	detach_ingress(st, &mq);

	while (!TAILQ_EMPTY(&mq)) {
		mblk_t *mp = TAILQ_FIRST(&mq);
		TAILQ_REMOVE(&mq, mp, link);

		str_put(st->rq_bottom, mp);
	}
}

static inline void
service_queues(stdata_t *st)
{
	queue_t *wq;
	queue_t *last_wq = st->rq_bottom->other;

	for (wq = st->wq; wq != NULL; wq = wq->next) {
		queue_t *rq = wq->other;

		if (atomic_exchange(&wq->enabled, 0)) {
			kassert(wq->qinfo->srvp != NULL);
			wq->qinfo->srvp(wq);
		}

		if (atomic_exchange(&rq->enabled, 0)) {
			kassert(rq->qinfo->srvp != NULL);
			rq->qinfo->srvp(rq);
		}

		if (wq == last_wq)
			break;
	}
}

static void
drain(struct str_per_cpu_scheduler *sc)
{
	for (;;) {
		stdata_t *st;
		uint32_t flags;

		ke_spinlock_enter_nospl(&sc->lock);

		while (!TAILQ_EMPTY(&sc->freeq)) {
			st = TAILQ_FIRST(&sc->freeq);
			TAILQ_REMOVE(&sc->freeq, st, sched_link);
			kmem_free(st, sizeof(*st));
		}

		st = TAILQ_FIRST(&sc->runq);
		if (st == NULL) {
			ke_spinlock_exit_nospl(&sc->lock);
			break;
		}

		TAILQ_REMOVE(&sc->runq, st, sched_link);
		atomic_fetch_and(&st->flags, ~ST_QUEUED);

		ke_spinlock_exit_nospl(&sc->lock);

		flags = atomic_load(&st->flags);

		if (flags & (ST_FROZEN | ST_DEAD))
			continue;

		if (!ke_mutex_tryenter(st->mutex)) {
			atomic_fetch_or(&st->flags, ST_NEEDRUN);
			continue;
		}

		atomic_fetch_or(&st->flags, ST_RUNNING);

		for (;;) {
			atomic_fetch_and(&st->flags, ~ST_NEEDRUN);

			drain_ingress(st);
			service_queues(st);

			flags = atomic_load(&st->flags);

			if (flags & (ST_FROZEN | ST_DEAD))
				break;

			if (!(flags & ST_NEEDRUN))
				break;
		}

		atomic_fetch_and(&st->flags, ~ST_RUNNING);

		str_unlock(st);
	}
}

static void
worker(void *arg)
{
	struct str_per_cpu_scheduler *sc = arg;

	for (;;) {
		ke_wait1(&sc->event, "streams worker wait", false,
		    ABSTIME_FOREVER);
		ke_event_set_signalled(&sc->event, false);
		drain(sc);
	}
}

void
str_unlock(stdata_t *st)
{
	uint32_t flags;

	ke_mutex_exit(st->mutex);

	flags = atomic_load(&st->flags);

	if (flags & (ST_FROZEN | ST_DEAD))
		return;

	if (flags & ST_NEEDRUN)
		str_kick(st);
}

/* Freezes a stream. Its mutex must be held. */
void
str_freeze(stdata_t *st)
{
	atomic_fetch_or(&st->flags, ST_FROZEN);
}

/* Thaws a stream. Its mutex must be held. (Will be kicked on str_unlock()) */
void
str_thaw(stdata_t *st)
{
	atomic_fetch_and(&st->flags, ~ST_FROZEN);
}

void
str_qenable(queue_t *q)
{
	kassert(q->qinfo != NULL);
	kassert(q->qinfo->srvp != NULL);

	/* nothing to do if already enabled */
	if (atomic_exchange(&q->enabled, 1) != 0)
		return;

	str_kick(q->stdata);
}


void
str_ingress_putq(stdata_t *st, mblk_t *mp)
{
	ipl_t ipl = ke_spinlock_enter(&st->ingress_lock);
	TAILQ_INSERT_TAIL(&st->ingress_head, mp, link);
	ke_spinlock_exit(&st->ingress_lock, ipl);
	str_kick(st);
}

void
str_sched_init(void)
{
	for (kcpunum_t i = 0; i < ke_ncpu; i++) {
		struct str_per_cpu_scheduler *sc = kmem_alloc(sizeof(*sc));
		ke_spinlock_init(&sc->lock);
		TAILQ_INIT(&sc->runq);
		TAILQ_INIT(&sc->freeq);
		ke_event_init(&sc->event, false);
		sc->worker = proc_new_system_thread(worker, sc);
		ke_cpu_data[i]->str_scheduler = sc;
		ke_thread_set_affinity(&sc->worker->kthread, i);
		ke_thread_resume(&sc->worker->kthread, false);
	}
}
