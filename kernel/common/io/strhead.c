/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file strhead.c
 * @brief Stream head operations.
 */

#include <sys/stream.h>
#include <sys/kmem.h>
#include <sys/strsubr.h>

static struct streamtab sth_streamtab;

static queue_t *
qpair_alloc(stdata_t *s, struct streamtab *tab)
{
	queue_t *rq, *wq;

	rq = kmem_alloc(sizeof(*rq));
	if (rq == NULL)
		return NULL;

	wq = kmem_alloc(sizeof(*wq));
	if (wq == NULL) {
		kmem_free(rq, sizeof(*rq));
		return NULL;
	}

	rq->is_readq = true;
	wq->is_readq = false;

	rq->enabled = wq->enabled = false;
	rq->wantw = wq->wantw = false;
	rq->full = wq->full = false;

	rq->other = wq;
	wq->other = rq;

	rq->mutex = wq->mutex = s->mutex;
	TAILQ_INIT(&rq->msgq);
	TAILQ_INIT(&wq->msgq);
	rq->count = wq->count = 0;
	rq->ptr = wq->ptr = NULL;
	rq->next = wq->next = NULL;
	rq->back = wq->back = NULL;

	rq->qinfo = tab->rinit;
	wq->qinfo = tab->winit;

	return rq;
}

static void
qpair_free(queue_t *rq)
{
	queue_t *wq = rq->other;
	kmem_free(rq, sizeof(*rq));
	kmem_free(wq, sizeof(*wq));
}

static int
qpair_open(queue_t *rq, void *dev)
{
	queue_t *wq = rq->other;

	if (rq->qinfo->qopen != NULL) {
		if (rq->qinfo->qopen(rq, dev) != 0)
			return -1;
	}

	if (wq->qinfo->qopen != NULL) {
		if (wq->qinfo->qopen(wq, dev) != 0)
			return -1;
	}

	return 0;
}

stdata_t *
str_head_alloc(enum str_head_kind kind)
{
	stdata_t *sh;

	sh = kmem_alloc(sizeof(*sh));
	if (sh == NULL)
		return NULL;

	sh->kind = kind;
	ke_mutex_init(&sh->integral_mutex);
	sh->mutex = &sh->integral_mutex;
	sh->rq_bottom = NULL;

	ke_spinlock_init(&sh->ingress_lock);
	TAILQ_INIT(&sh->ingress_head);

	sh->flags = 0;
	sh->home_cpu = CPU_LOCAL_LOAD(cpu_num);

	sh->req_locked = false;
	TAILQ_INIT(&sh->req_waiters);
	sh->hanged_up = false;
	ke_event_init(&sh->data_readable, 0);
	ke_event_init(&sh->ioctl_done_ev, 0);
	sh->read_mode = STR_RNORM;

#if 0
	pollhead_init(&sh->pollhead);
#endif

	if (kind == STR_HEAD_KIND_TTY) {
		sh->tty_pgrp = NULL;
		sh->tty_session = NULL;
	} else if (kind == STR_HEAD_KIND_RPIPE ||
		   kind == STR_HEAD_KIND_WPIPE) {
		sh->pipe_peer = NULL;
		sh->write_broken = false;
	}

	sh->rq = qpair_alloc(sh, &sth_streamtab);
	if (sh->rq == NULL) {
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}
	sh->wq = sh->rq->other;
	sh->rq->ptr = sh->wq->ptr = sh;

	return sh;
}

stdata_t *
stropen(struct streamtab *devtab, void *dev)
{
	stdata_t *sh;
	queue_t *devrq;

	sh = str_head_alloc(STR_HEAD_KIND_TTY);
	if (sh == NULL)
		return NULL;

	sh->devtab = devtab;

	devrq = qpair_alloc(sh, devtab);
	if (devrq == NULL) {
		qpair_free(sh->rq);
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}

	devrq->next = sh->rq;
	sh->rq->back = devrq;
	sh->wq->next = devrq->other;
	devrq->other->back = sh->wq;
	sh->rq_bottom = devrq;

	if (qpair_open(sh->rq, dev) != 0 || qpair_open(devrq, dev) != 0) {
		qpair_free(devrq);
		qpair_free(sh->rq);
		kmem_free(sh, sizeof(*sh));
		return NULL;
	}

	return sh;
}

static void
sth_rput(queue_t *q, mblk_t *mp)
{
	stdata_t *sh = (stdata_t *)q->ptr;

	switch (mp->db->type) {
	case M_DATA:

		q->count += str_msgsize(mp);

		/* flow control (c.f. str_putq) */
		if (q->count > q->hiwat)
			q->full = true;

		ke_event_set_signalled(&sh->data_readable, true);
		TAILQ_INSERT_TAIL(&sh->rq->msgq, mp, link);
#if 0
		pollhead_deliver_events(&sh->pollhead, EPOLLIN | EPOLLRDNORM);
#endif
		break;

	case M_HANGUP:
		str_freeb(mp);
		sh->hanged_up = true;
#if 0
		pollhead_deliver_events(&sh->pollhead, EPOLLHUP);
#endif

#if 0
		if (sh->tty_pgrp != NULL)
			kdprintf(" = TODO str_head: SIGHUP to pgrp %d\n",
			    sh->tty_pgrp->pgid);
#endif

		break;

	case M_SETOPTS: {
		struct stroptions *sop = (struct stroptions *)mp->rptr;

		str_freeb(mp);
		break;
	}

	case M_IOCACK:
		ke_event_set_signalled(&sh->ioctl_done_ev, true);
		break;

	default:
		kfatal("sth_rput: unhandled message type %d\n", mp->db->type);
	}
}

static void
sth_wput(queue_t *q, mblk_t *mp)
{
	kassert(mp->db->type == M_DATA || mp->db->type == M_IOCTL);
	str_putnext(q->other, mp);
}

static void
sth_wsrv(queue_t *q)
{
	kfatal("TODO: sth_wsrv: may unblock writers, wake pollers\n");
}

static struct qinit rinit = {
	.putp = sth_rput,
	.hiwat = 536,
	.lowat = 128,
};

static struct qinit winit = {
	.putp = sth_wput,
	.srvp = sth_wsrv,
};

static struct streamtab sth_streamtab = {
	.rinit = &rinit,
	.winit = &winit,
};
