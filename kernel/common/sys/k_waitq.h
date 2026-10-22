/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file k_waitq.h
 * @brief Wait queues with chained stack wait blocks.
 *
 * Something like a condvar:
 * - thread checks a condition under some lock relevant to it;
 * - it registers a waiter, allocated on its stack, with ke_waitq_prepare(),
 *   while holding that lock;
 * - it drops that lock and calls ke_wait1_wait();
 * - whoever changes the condition does it under the same lock and wakes the
 *   queue.
 *
 * For uninterruptible waits with no deadline the waker never touches the queue
 * again so the waker can free the object embedding the queue after waking
 * everyone.
 *
 * Otherwise the waiter has to take the kwaitq::lock to unlink itself, so the
 * waiter has to have some existence guarantee on the object. If there is a race
 * between wakeup and interruption then the wake is returned.
 *
 * If a woken waiter wants to free the object embedding the queue it should
 * call ke_waitq_quiesce() (which takes the kwaitq::lock) in case the waker is
 * still inside it when the waiter has run.
 */

#ifndef ECX_KEYRONEX_K_WAITQ_H
#define ECX_KEYRONEX_K_WAITQ_H

#include <sys/k_wait.h>

#include <stdbool.h>

typedef struct kwaiter {
	TAILQ_ENTRY(kwaiter) link;
	kevent_t event;
	bool queued; /* kwaitq::lock */
} kwaiter_t;

typedef struct kwaitq {
	kspinlock_t lock;
	TAILQ_HEAD(, kwaiter) waiters;
} kwaitq_t;

#define KWAITQ_INITIALISER(WQ) {				\
	.lock = KSPINLOCK_INITIALISER,				\
	.waiters = TAILQ_HEAD_INITIALIZER((WQ).waiters)		\
}

void ke_waitq_init(kwaitq_t *wq);
void ke_waitq_prepare(kwaitq_t *wq, kwaiter_t *waiter);
int ke_waitq_wait(kwaitq_t *wq, kwaiter_t *waiter, const char *reason,
    bool alertable, kabstime_t deadline);
void ke_waitq_wake_all(kwaitq_t *wq);
bool ke_waitq_wake_one(kwaitq_t *wq);
void ke_waitq_quiesce(kwaitq_t *wq);

#endif /* ECX_KEYRONEX_K_WAITQ_H */
