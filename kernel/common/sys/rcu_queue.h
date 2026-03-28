/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Mar 27 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file rcu_queue.h
 * @brief <sys/queue.h> style lists safe for RCU readers.
 *
 * Writers still need to be serialised and elements must be RCU-freed.
 */

#ifndef ECX_SYS_RCU_QUEUE_H
#define ECX_SYS_RCU_QUEUE_H

#include <stdatomic.h>

#include <sys/k_rcu.h>

#define _RCU(type) type

#define	RCULIST_HEAD(name, type)					\
struct name {								\
	_RCU(struct type *)	rlh_first;				\
}

#define	RCULIST_HEAD_INITIALIZER(head)					\
	{ NULL }

#define	RCULIST_ENTRY(type)						\
struct {								\
	_RCU(struct type *)	rle_next;				\
	_RCU(struct type *)	*rle_prev;				\
}

/* reader's API */

#define RCULIST_FIRST(head)	(ke_rcu_dereference(&(head)->rlh_first))

#define	RCULIST_EMPTY(head)	(RCULIST_FIRST(head) == NULL)

#define RCULIST_NEXT(elm, field) (ke_rcu_dereference(&(elm)->field.rle_next))

#define RCULIST_FOREACH(var, head, field)				\
	for ((var) = RCULIST_FIRST((head));				\
	   (var);							\
	   (var) = RCULIST_NEXT((var), field))

#define RCULIST_FOREACH_SAFE(var, head, field, tvar)			\
	for ((var) = RCULIST_FIRST((head));				\
	   (var) && ((tvar) = RCULIST_NEXT((var), field), 1); 		\
	   (var) = (tvar))

/* writer's API */

#define	RCULIST_INIT(head) do {						\
	ke_rcu_assign_pointer(&(head)->rlh_first, NULL);		\
} while (0)

#define RCULIST_INSERT_HEAD(head, elm, field) do {			\
	__auto_type old = RCULIST_FIRST(head);				\
	ke_rcu_assign_pointer(&(elm)->field.rle_next, old);		\
	(elm)->field.rle_prev = &(head)->rlh_first;			\
	if (old != NULL)						\
		old->field.rle_prev = &(elm)->field.rle_next;		\
	ke_rcu_assign_pointer(&(head)->rlh_first, (elm));		\
} while (0)

#define RCULIST_REMOVE(elm, field) do {					\
	__auto_type _next = RCULIST_NEXT((elm), field);			\
	ke_rcu_assign_pointer((elm)->field.rle_prev, _next);		\
	if (_next != NULL)						\
		_next->field.rle_prev = (elm)->field.rle_prev;		\
} while (0)

#endif /* ECX_SYS_RCU_QUEUE_H */
