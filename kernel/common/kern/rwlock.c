/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file rwlock.c
 * @brief Reader-writer lock.
 */

#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/k_intr.h>

#define RW_WLOCKED	0x1UL
#define RW_WAITERS	0x2UL
#define RW_FLAGMASK	0x3UL
#define RW_1READER	0x4UL		/* one reader = 1 << 2 */

#define RW_READERS(V)	((V) >> 2)
#define RW_OWNER(V) ((V & RW_WLOCKED) ? (kthread_t*)(val & ~RW_FLAGMASK) : NULL)

void
ke_rwlock_init(krwlock_t *rw)
{
	rw->val = 0;
}

void
ke_rwlock_enter_read(krwlock_t *rw, const char *reason)
{
	kturnstile_t *ts;
	uintptr_t val, new;
	ipl_t ipl;

	for (;;) {
		retry:
		val = __atomic_load_n(&rw->val, __ATOMIC_RELAXED);

		if (likely((val & RW_WLOCKED) == 0)) {
			new = val + RW_1READER;
			if (__atomic_compare_exchange_n(&rw->val, &val, new,
			    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
				return;
			continue;
		}

		/* It's Write-locked. Block on the turnstile. */
		ipl = ke_turnstile_lookup(rw, &ts);

		val = __atomic_load_n(&rw->val, __ATOMIC_RELAXED);
		if ((val & RW_WLOCKED) == 0) {
			ke_turnstile_exit(ts, ipl);
			continue;
		}

		while (!(val & RW_WAITERS)) {
			new = val | RW_WAITERS;
			if (__atomic_compare_exchange_n(&rw->val, &val, new,
			    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				break;
			if ((val & RW_WLOCKED) == 0) {
				ke_turnstile_exit(ts, ipl);
				goto retry;
			}
		}

		ke_turnstile_block(ts, false, rw, RW_OWNER(val), ipl);
		return;
	}
}

void
ke_rwlock_enter_write(krwlock_t *rw, const char *reason)
{
	kthread_t *self = ke_curthread();
	kturnstile_t *ts;
	uintptr_t val, new;
	ipl_t ipl;

	for (;;) {
		retry:
		val = __atomic_load_n(&rw->val, __ATOMIC_RELAXED);

		if (likely(val == 0)) {
			new = (uintptr_t)self | RW_WLOCKED;
			if (__atomic_compare_exchange_n(&rw->val, &val, new,
			    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
				return;
			continue;
		}

		/* Locked (read or write). Block on the turnstile. */
		ipl = ke_turnstile_lookup(rw, &ts);

		val = __atomic_load_n(&rw->val, __ATOMIC_RELAXED);
		if (val == 0) {
			ke_turnstile_exit(ts, ipl);
			continue;
		}

		while (!(val & RW_WAITERS)) {
			new = val | RW_WAITERS;
			if (__atomic_compare_exchange_n(&rw->val, &val, new,
			    false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
				break;
			if (val == 0) {
				ke_turnstile_exit(ts, ipl);
				goto retry;
			}
		}

		ke_turnstile_block(ts, true, rw, RW_OWNER(val), ipl);
		return;
	}
}

void
ke_rwlock_exit_read(krwlock_t *rw)
{
	kturnstile_t *ts;
	uintptr_t val, new;
	ipl_t ipl;

	for (;;) {
		val = __atomic_load_n(&rw->val, __ATOMIC_RELAXED);

		kassert((val & RW_WLOCKED) == 0 && RW_READERS(val) > 0);

		/*
		 * If we are not the last reader, or if there are no waiters,
		 * we can just try to decrement the reader count.
		 */
		if (likely(RW_READERS(val) > 1 || !(val & RW_WAITERS))) {
			new = val - RW_1READER;
			if (__atomic_compare_exchange_n(&rw->val, &val, new,
			    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
				return;
			continue;
		}

		/*
		 * We were the last reader and there are waiters (must be
		 * writers as readers would just acquire it.)
		 */
		ipl = ke_turnstile_lookup(rw, &ts);
		kassert(ts != NULL && ts->nwaiters[1] > 0);

		new = (uintptr_t)ke_turnstile_waiter(ts, true) | RW_WLOCKED;
		if ((ts->nwaiters[0] + ts->nwaiters[1]) > 1)
			new |= RW_WAITERS;

		/* CAS in case another reader arrived meanwhile. */
		if (__atomic_compare_exchange_n(&rw->val, &val, new, false,
			__ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
			ke_turnstile_wakeup(ts, true, 1, NULL, ipl);
			return;
		}

		ke_turnstile_exit(ts, ipl);
	}
}

void
ke_rwlock_exit_write(krwlock_t *rw)
{
	kthread_t *self = ke_curthread();
	kturnstile_t *ts;
	uintptr_t val, new;
	ipl_t ipl;
	int nreaders;

	/*
	 * Try to CAS from locked-with-no-waiters to unlocked, if we can.
	 * If CAS fails then the WAITERS bit must have been set.
	 */
	val = (uintptr_t)self | RW_WLOCKED;
	if (likely(__atomic_compare_exchange_n(&rw->val, &val, 0,
	    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)))
		return;

	kassert(val == ((uintptr_t)self | RW_WLOCKED | RW_WAITERS));

	ipl = ke_turnstile_lookup(rw, &ts);
	kassert(ts != NULL);

	if (ts->nwaiters[0] > 0) {
		/* Wake all the waiting readers with a direct handoff. */
		nreaders = ts->nwaiters[0];
		new = (uintptr_t)nreaders * RW_1READER;
		if (ts->nwaiters[1] > 0)
			new |= RW_WAITERS;

		__atomic_store_n(&rw->val, new, __ATOMIC_RELEASE);
		ke_turnstile_wakeup(ts, false, nreaders, NULL, ipl);
	} else {
		/* Direct handoff to the first writer. */
		new = (uintptr_t)ke_turnstile_waiter(ts, true) | RW_WLOCKED;
		if (ts->nwaiters[1] > 1)
			new |= RW_WAITERS;

		__atomic_store_n(&rw->val, new, __ATOMIC_RELEASE);
		ke_turnstile_wakeup(ts, true, 1, NULL, ipl);
	}
}

void
ke_rwlock_downgrade(krwlock_t *rw)
{
	kthread_t *self = ke_curthread();
	kturnstile_t *ts;
	uintptr_t val, new;
	ipl_t ipl;
	int nreaders;

	/* Try to CAS from write-locked-no-waiters to 1-reader, if we can. */
	val = (uintptr_t)self | RW_WLOCKED;
	new = RW_1READER;
	if (likely(__atomic_compare_exchange_n(&rw->val, &val, new,
	    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)))
		return;

	/* Otherwise there must be waiters. */
	kassert(val == ((uintptr_t)self | RW_WLOCKED | RW_WAITERS));

	ipl = ke_turnstile_lookup(rw, &ts);
	kassert(ts != NULL);

	if (ts->nwaiters[0] > 0) {
		/* The waiting readers join us in holding the rwlock read. */
		nreaders = ts->nwaiters[0];
		new = (uintptr_t)(nreaders + 1) * RW_1READER;
		if (ts->nwaiters[1] > 0)
			new |= RW_WAITERS;

		__atomic_store_n(&rw->val, new, __ATOMIC_RELEASE);
		ke_turnstile_wakeup(ts, false, nreaders, NULL, ipl);
	} else {
		/*
		 * There are only writers waiting. Become a single reader.
		 * WAITERS stays set so we can wake them when we release.
		 */
		new = RW_1READER | RW_WAITERS;
		__atomic_store_n(&rw->val, new, __ATOMIC_RELEASE);
		ke_turnstile_exit(ts, ipl);
	}
}

bool
ke_rwlock_tryupgrade(krwlock_t *rw)
{
	kthread_t *self = ke_curthread();
	uintptr_t val, new;

	val = __atomic_load_n(&rw->val, __ATOMIC_RELAXED);

	for (;;) {
		kassert((val & RW_WLOCKED) == 0 && RW_READERS(val) >= 1);

		/* Others readers exist. Upgrade impossible without blocking. */
		if (RW_READERS(val) > 1)
			return false;

		/* Sole reader. CAS to write-locked, preserving WAITERS. */
		new = (uintptr_t)self | RW_WLOCKED | (val & RW_WAITERS);
		if (__atomic_compare_exchange_n(&rw->val, &val, new,
		    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
			return true;

		/* Another reader arrived (or WAITERS was added). Go again */
	}
}
