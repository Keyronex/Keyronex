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
#include <sys/stream.h>

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
