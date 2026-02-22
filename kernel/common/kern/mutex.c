/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file mutex.c
 * @brief Mutex.
 *
 * TODO:
 * - adaptive spinning
 */

#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/k_intr.h>

#define MTX_LOCKED	0x1UL
#define MTX_WAITERS	0x2UL
#define MTX_FLAGMASK	0x3UL

#define MTX_OWNER(V)	((kthread_t *)((V) & ~MTX_FLAGMASK))

void
ke_mutex_init(kmutex_t *mtx)
{
	mtx->val = 0;
}

void
ke_mutex_enter(kmutex_t *mtx, const char *)
{
	kthread_t *self = ke_curthread();
	kturnstile_t *ts;
	uintptr_t val, new;
	ipl_t ipl;

	for (;;) {
		retry:
		val = __atomic_load_n(&mtx->val, __ATOMIC_RELAXED);

		if (likely(val == 0)) {
			new = (uintptr_t)self | MTX_LOCKED;
			if (__atomic_compare_exchange_n(&mtx->val, &val, new,
			    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
				return;
			continue;
		}

		ipl = ke_turnstile_lookup(mtx, &ts);

		val = __atomic_load_n(&mtx->val, __ATOMIC_RELAXED);
		if (val == 0) {
			ke_turnstile_exit(ts, ipl);
			continue;
		}

		while (!(val & MTX_WAITERS)) {
			new = val | MTX_WAITERS;
			if (__atomic_compare_exchange_n(&mtx->val, &val, new,
			    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				break;
			if (val == 0) {
				ke_turnstile_exit(ts, ipl);
				goto retry;
			}
		}

		ke_turnstile_block(ts, true, mtx, MTX_OWNER(val), ipl);
		/* note: no ownership handoff */
	}
}

bool
ke_mutex_tryenter(kmutex_t *mtx)
{
	kthread_t *self = ke_curthread();
	uintptr_t val = 0, new;

	new = (uintptr_t)self | MTX_LOCKED;
	return __atomic_compare_exchange_n(&mtx->val, &val, new,
	    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

void
ke_mutex_exit(kmutex_t *mtx)
{
	kthread_t *self = ke_curthread();
	kturnstile_t *ts;
	uintptr_t val;
	ipl_t ipl;

	val = (uintptr_t)self | MTX_LOCKED;
	if (likely(__atomic_compare_exchange_n(&mtx->val, &val, 0,
	    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)))
		return;

	kassert(val == ((uintptr_t)self | MTX_LOCKED | MTX_WAITERS));

	ipl = ke_turnstile_lookup(mtx, &ts);
	kassert(ts != NULL);

	__atomic_store_n(&mtx->val, 0, __ATOMIC_RELEASE);
	ke_turnstile_wakeup(ts, true, ts->nwaiters[1], NULL, ipl);
}

bool
ke_mutex_held(kmutex_t *mtx)
{
	kthread_t *self = ke_curthread();
	uintptr_t val = __atomic_load_n(&mtx->val, __ATOMIC_RELAXED);
	return MTX_OWNER(val) == self;
}
