/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file strsubr.c
 * @brief STREAMS logic.
 */

#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/stream.h>
#include <sys/strsubr.h>

static void
dblk_release(dblk_t *db)
{
	if (atomic_fetch_sub_explicit(&db->refcnt, 1, memory_order_acq_rel) == 1) {
		kmem_free(db->base, db->lim - db->base);
		kmem_free(db, sizeof(*db));
	}
}

mblk_t *
str_allocb(size_t size)
{
	mblk_t *mp;
	dblk_t *db;
	char *data = NULL;

	if (size != 0) {
		data = kmem_alloc(size);
		if (data == NULL)
			return NULL;
	}

	db = kmem_alloc(sizeof(*db));
	if (db == NULL) {
		kmem_free(data, size);
		return NULL;
	}

	mp = kmem_alloc(sizeof(*mp));
	if (mp == NULL) {
		kmem_free(data, size);
		kmem_free(db, sizeof(*db));
		return NULL;
	}

	atomic_init(&db->refcnt, 1);
	db->type = M_DATA;
	db->base = data;
	db->lim = data + size;

	mp->db = db;
	mp->rptr = data;
	mp->wptr = data;
	mp->cont = NULL;

	return mp;
}

void
str_freeb(mblk_t *mp)
{
	if (mp == NULL)
		return;

	if (mp->db != NULL)
		dblk_release(mp->db);

	kmem_free(mp, sizeof(*mp));
}

void
str_freemsg(mblk_t *mp)
{
	mblk_t *next;

	while (mp != NULL) {
		next = mp->cont;
		str_freeb(mp);
		mp = next;
	}
}

mblk_t *
str_copyb(mblk_t *mp)
{
	mblk_t *nmp;

	if (mp == NULL)
		return NULL;

	nmp = str_allocb(mp->wptr - mp->rptr);
	if (nmp == NULL)
		return NULL;

	memcpy(nmp->rptr, mp->rptr, mp->wptr - mp->rptr);
	nmp->wptr = nmp->rptr + (mp->wptr - mp->rptr);

	return nmp;
}

mblk_t *
str_copymsg(mblk_t *mp)
{
	mblk_t *nmp, *head = NULL, *prev = NULL;

	while (mp != NULL) {
		nmp = str_copyb(mp);
		if (nmp == NULL) {
			str_freemsg(head);
			return NULL;
		}

		if (prev != NULL)
			prev->cont = nmp;
		else
			head = nmp;

		prev = nmp;
		mp = mp->cont;
	}

	return head;
}

size_t
str_msgsize(mblk_t *mp)
{
	size_t size = 0;
	while (mp != NULL) {
		if (mp->wptr > mp->rptr)
			size += mp->wptr - mp->rptr;
		mp = mp->cont;
	}
	return size;
}

void
str_put(queue_t *q, mblk_t *mp)
{
	q->qinfo->putp(q, mp);
}

void
str_putnext(queue_t *q, mblk_t *mp)
{
	kassert(q->next != NULL);
	str_put(q->next, mp);
}

void
str_qreply(queue_t *q, mblk_t *mp)
{
	str_putnext(q->other, mp);
}

bool
str_canput(queue_t *q)
{
	while (q->next != NULL && q->qinfo->srvp == NULL)
		q = q->next;

	if (q->full) {
		q->wantw = true;
		return false;
	}

	return true;
}

bool
str_canputnext(queue_t *q)
{
	kassert(q->next != NULL);
	return str_canput(q->next);
}

void
str_putq(queue_t *q, mblk_t *mp)
{
	bool was_empty = TAILQ_EMPTY(&q->msgq);
	size_t size = str_msgsize(mp);

	kassert(ke_mutex_held(q->stdata->mutex));

	if (mp->db->type >= M_SETOPTS) /* first hi-priority */
		TAILQ_INSERT_HEAD(&q->msgq, mp, link);
	else
		TAILQ_INSERT_TAIL(&q->msgq, mp, link);

	q->count += size;

	if (q->count >= q->hiwat)
		q->full = true;

	if (was_empty && q->qinfo->srvp != NULL)
		str_qenable(q);
}

void
str_putbq(queue_t *q, mblk_t *mp)
{
	bool was_empty = TAILQ_EMPTY(&q->msgq);
	size_t size = str_msgsize(mp);

	kassert(ke_mutex_held(q->stdata->mutex));

	TAILQ_INSERT_HEAD(&q->msgq, mp, link);

	q->count += size;

	if (q->count >= q->hiwat)
		q->full = true;

	if (was_empty && q->qinfo->srvp != NULL)
		str_qenable(q);
}

void
str_backenable(queue_t *q)
{
	queue_t *bq = q->back;

	while (bq != NULL && bq->qinfo->srvp == NULL)
		bq = bq->back;

	if (bq != NULL)
		str_qenable(bq);
}

mblk_t *
str_getq(queue_t *q)
{
	mblk_t *mp = TAILQ_FIRST(&q->msgq);
	if (mp == NULL)
		return NULL;

	kassert(ke_mutex_held(q->stdata->mutex));

	TAILQ_REMOVE(&q->msgq, mp, link);
	q->count -= str_msgsize(mp);

	if (q->full && q->count <= q->lowat) {
		q->full = false;
		if (q->wantw) {
			q->wantw = false;
			str_backenable(q);
		}
	}

	return mp;
}

void
str_flushq(queue_t *q, int flag)
{
	kassert(flag == FLUSHALL);
	while (!TAILQ_EMPTY(&q->msgq)) {
		mblk_t *mp = TAILQ_FIRST(&q->msgq);
		TAILQ_REMOVE(&q->msgq, mp, link);
		str_freemsg(mp);
	}
	q->count = 0;
}

void
str_mblk_q_free(mblk_q_t *q)
{
	mblk_t *mblk, *next;

	TAILQ_FOREACH_SAFE(mblk, q, link, next) {
		TAILQ_REMOVE(q, mblk, link);
		str_freemsg(mblk);
	}
}
