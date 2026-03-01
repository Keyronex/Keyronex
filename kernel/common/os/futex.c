/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sun Mar 01 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file futex.c
 * @brief Linux-style fast userspace mutexes.
 */

#include <sys/k_log.h>
#include <sys/proc.h>
#include <sys/vm.h>
#include <sys/errno.h>
#include <sys/k_thread.h>
#include <sys/libkern.h>

struct futex_waiter {
	TAILQ_ENTRY(futex_waiter) tq_entry;
	struct vm_voaddr voaddr;
	kevent_t event;
	bool queued;
};

static TAILQ_HEAD(futex_waiter_list, futex_waiter) futex_waiters =
    TAILQ_HEAD_INITIALIZER(futex_waiters);
static kmutex_t futex_mutex = KMUTEX_INITIALISER;

int
sys_futex_wait(int *u_pointer, int expected, const struct timespec *user_ts)
{
	struct vm_voaddr voaddr;
	struct timespec ts;
	kabstime_t deadline = ABSTIME_FOREVER;
	struct futex_waiter waiter;
	int value;
	int r;

	if (user_ts != NULL) {
		r = memcpy_from_user(&ts, user_ts, sizeof(ts));
		if (r != 0)
			return r;

		deadline = ke_time() + ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	}

	r = vm_voaddr_acquire(thread_vm_map(curthread()), (uintptr_t)u_pointer,
	    &voaddr);
	if (r != 0)
		return r;

	ke_mutex_enter(&futex_mutex, "futex_wait:lock");

	r = memcpy_from_user(&value, u_pointer, sizeof(int));
	if (r != 0) {
		ke_mutex_exit(&futex_mutex);
		vm_voaddr_release(thread_vm_map(curthread()), &voaddr);
		return r;
	}

	if (value != expected) {
		ke_mutex_exit(&futex_mutex);
		vm_voaddr_release(thread_vm_map(curthread()), &voaddr);
		return -EAGAIN;
	}

	waiter.queued = true;
	waiter.voaddr = voaddr;
	ke_event_init(&waiter.event, false);

	TAILQ_INSERT_TAIL(&futex_waiters, &waiter, tq_entry);

	ke_mutex_exit(&futex_mutex);

	r = ke_wait1(&waiter.event, "futex_wait", true, deadline);
	switch (r) {
	case 0:
		kassert(!waiter.queued);
		break;

	case -EINTR:
		kdprintf("futex_wait: note mlibc may not handle EINTR here\n");
		/* fall through */
	case -ETIMEDOUT:
		ke_mutex_enter(&futex_mutex, "futex_wait:remove");
		if (waiter.queued)
			TAILQ_REMOVE(&futex_waiters, &waiter, tq_entry);
		ke_mutex_exit(&futex_mutex);
		break;

	default:
		kfatal("unexpected synch_wait1 return %d\n", r);
	}

	vm_voaddr_release(thread_vm_map(curthread()), &voaddr);

	return r;
}

int
sys_futex_wake(int *u_pointer, int count)
{
	struct vm_voaddr voaddr;
	struct futex_waiter *waiter, *tmp;
	int r;
	int woken = 0;

	r = vm_voaddr_acquire(thread_vm_map(curthread()), (uintptr_t)u_pointer,
	    &voaddr);
	if (r != 0)
		return r;

	ke_mutex_enter(&futex_mutex, "futex_wake:lock");

	TAILQ_FOREACH_SAFE(waiter, &futex_waiters, tq_entry, tmp) {
		if (vm_voaddr_cmp(&waiter->voaddr, &voaddr) == 0) {
			TAILQ_REMOVE(&futex_waiters, waiter, tq_entry);
			waiter->queued = false;
			ke_event_set_signalled(&waiter->event, true);
			woken++;
			if (woken >= count)
				break;
		}
	}

	ke_mutex_exit(&futex_mutex);
	vm_voaddr_release(thread_vm_map(curthread()), &voaddr);

	return 0;
}
