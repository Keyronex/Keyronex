/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file kwait.h
 * @brief Kernel waitable objects.
 */

#ifndef ECX_KEYRONEX_KWAIT_H
#define ECX_KEYRONEX_KWAIT_H

#include <libkern/queue.h>

#include <keyronex/intr.h>
#include <keyronex/ktypes.h>

TAILQ_HEAD(kwaitblock_queue, kwaitblock);

enum kwaitblock_status {
	WAITBLOCK_ACTIVE,
	WAITBLOCK_ACQUIRED,
	WAITBLOCK_INACTIVE,
};

struct kwaitblock {
	TAILQ_ENTRY(kwaitblock) qlink;
	enum kwaitblock_status status;
	struct kthread *waiter;
};

enum kwait_internal_status {
	SYNCH_PRE_WAIT,
	SYNCH_WAIT,
	SYNCH_POST_WAIT,
} ;

typedef _Atomic(enum kwait_internal_status) kwait_internal_status_t;

enum synch_type {
	SYNCH_EVENT,
	SYNCH_CALLOUT,
	SYNCH_SEMAPHORE,
};

struct kdispatch_header {
	int signalled;
	enum synch_type type : 3;
	kspinlock_t lock;
	struct kwaitblock_queue waitq;
};

#define KDISPATCH_HEADER_INITIALISER(HDR, TYPE, SIGNALED) {	\
	.lock = SPINLOCK_INITIALISER,				\
	.type = TYPE,						\
	.signalled = SIGNALED,					\
	.waitq = TAILQ_HEAD_INITIALIZER((HDR).waitq)		\
}

typedef struct kevent {
	struct kdispatch_header header;
} kevent_t;

typedef struct ksemaphore {
	struct kdispatch_header header;
} ksemaphore_t;

typedef struct kcallout {
	struct kdispatch_header header;
	TAILQ_ENTRY(kcallout) callout_qlink;
	_Atomic(kcpunum_t) cpu_num;
	kabstime_t deadline;
	kdpc_t *softint;
} kcallout_t;

struct kcpu_callout {
	kdpc_t expiry_dpc;
	kspinlock_t lock;
	TAILQ_HEAD(, kcallout) callouts;
	_Atomic(kabstime_t) next_deadline;
};

/* public interface */

int ke_wait1(void *obj, const char *reason, bool signallable,
    kabstime_t deadline);
int ke_waitn(size_t obj_n, void *obj[], const char *reason, bool signallable,
    kabstime_t deadline);

void ke_callout_init(kcallout_t *);
void ke_callout_init_dpc(kcallout_t *, kdpc_t *dpc,
    void (*func)(void *, void *), void *arg1, void *arg2);
int ke_callout_set(kcallout_t *, kabstime_t deadline);
int ke_callout_stop(kcallout_t *);

void ke_event_init(kevent_t *, bool signalled);
void ke_event_set_signalled(kevent_t *, bool signalled);

void ke_semaphore_init(ksemaphore_t *);
void ke_semaphore_signal(ksemaphore_t *, size_t increment);

/* private interface */

void kep_signal(struct kdispatch_header *obj, struct kwaitblock_queue *wakeq);
void kep_waiters_wake(struct kwaitblock_queue *wakeq);
void kep_dispatcher_obj_init(struct kdispatch_header *obj, int signal,
    enum synch_type type);

#endif /* ECX_KEYRONEX_KWAIT_H */
