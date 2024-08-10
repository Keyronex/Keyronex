#include "kdk/kern.h"
#include "kdk/queue.h"
#include "ki.h"

typedef int64_t time_t;

nanosecs_t
ke_get_local_nanos(kcpu_t *cpu)
{
#if PORT == virt68k
	return cpu->nanos;
#else
	return __atomic_load_n(&cpu->nanos, __ATOMIC_SEQ_CST);
#endif
}

void
ki_timer_enqueue_locked(ktimer_t *callout)
{
	ktimer_t *existing;
	kcpu_t *cpu;
	ipl_t ipl;

	ipl = ke_spinlock_acquire_at(&curcpu()->dpc_lock, kIPLHigh);
	cpu = curcpu();

	callout->cpu = cpu;
	__atomic_store_n(&callout->state, kTimerInQueue, __ATOMIC_RELEASE);

	TAILQ_FOREACH (existing, &cpu->timer_queue, queue_entry) {
		if (existing->deadline > callout->deadline) {
			TAILQ_INSERT_BEFORE(existing, callout, queue_entry);
			goto next;
		}
	}
	/* no callouts or it's the longest till elapsing */
	TAILQ_INSERT_TAIL(&cpu->timer_queue, callout, queue_entry);

next:
	ke_spinlock_release(&cpu->dpc_lock, ipl);
}

bool

ki_timer_dequeue_locked(ktimer_t *callout)
{
	kcpu_t *cpu;
	ipl_t ipl;

	if (__atomic_load_n(&callout->state, __ATOMIC_ACQUIRE) ==
	    kTimerDisabled)
		return true;

	/*
	 * we are at IPL = kDPC already, and callout->cpu will stay stable while
	 * we have the DPC lock held
	 */

	cpu = callout->cpu;
	ipl = ke_spinlock_acquire_at(&cpu->dpc_lock, kIPLHigh);

	if (__atomic_load_n(&callout->state, __ATOMIC_ACQUIRE) ==
	    kTimerExecuting) {
		ke_spinlock_release(&cpu->dpc_lock, ipl);
		return false;
	} else if (__atomic_load_n(&callout->state, __ATOMIC_ACQUIRE) ==
	    kTimerDisabled) {
		ke_spinlock_release(&cpu->dpc_lock, ipl);
		return true;
	}

	kassert(__atomic_load_n(&callout->state, __ATOMIC_ACQUIRE) ==
	    kTimerInQueue);
	kassert(callout->cpu == cpu);
	TAILQ_REMOVE(&cpu->timer_queue, callout, queue_entry);
	__atomic_store_n(&callout->state, kTimerDisabled, __ATOMIC_RELEASE);

	ke_spinlock_release(&cpu->dpc_lock, ipl);

	return true;
}

bool
ki_cpu_hardclock(md_intr_frame_t *frame, void *arg)
{
	ktimer_t *co;
	nanosecs_t nanos;
	kcpu_t *cpu;
	ipl_t ipl;
	bool want_timers = false;

	/* in principle we should already be at IPL=high */
	ipl = ke_spinlock_acquire_at(&curcpu()->dpc_lock, kIPLHigh);
	cpu = curcpu();
#if PORT == virt68k
	nanos = cpu->nanos += NS_PER_S / KERN_HZ;
#else
	nanos = __atomic_fetch_add(&cpu->nanos, NS_PER_S / KERN_HZ,
	    __ATOMIC_SEQ_CST);
#endif

	if (cpu->curthread->timeslice-- <= 0) {
		cpu->reschedule_reason = kRescheduleReasonPreempted;
		md_raise_dpc_interrupt();
	}

	co = TAILQ_FIRST(&cpu->timer_queue);

	if (co != NULL && co->deadline <= nanos)
		want_timers = true;

	ke_spinlock_release(&cpu->dpc_lock, ipl);

	if (want_timers)
		ke_dpc_enqueue(&cpu->timer_expiry_dpc);

	return true;
}

void
timer_expiry_dpc(void *arg)
{
	kcpu_t *cpu = arg;
	while (true) {
		ipl_t ipl = ke_spinlock_acquire_at(&cpu->dpc_lock, kIPLHigh);
		ktimer_t *timer = TAILQ_FIRST(&cpu->timer_queue);

		if (timer == NULL || timer->deadline > cpu->nanos) {
			ke_spinlock_release(&cpu->dpc_lock, ipl);
			break;
		}

		enum ktimer_state expected = kTimerInQueue;

		if (!__atomic_compare_exchange_n(&timer->state, &expected,
			kTimerExecuting, false, __ATOMIC_RELAXED,
			__ATOMIC_RELAXED)) {
			/* timer cancelled */
			ke_spinlock_release(&cpu->dpc_lock, ipl);
			continue;
		}

		TAILQ_REMOVE(&cpu->timer_queue, timer, queue_entry);
		ke_spinlock_release(&cpu->dpc_lock, ipl);

		kwaitblock_queue_t waiters_queue = TAILQ_HEAD_INITIALIZER(
		    waiters_queue);

		ke_spinlock_acquire_nospl(&timer->hdr.spinlock);
		timer->hdr.signalled = 1;
		ki_signal(&timer->hdr, &waiters_queue);
		timer->hdr.signalled = 0;
		if (timer->dpc != NULL)
			ke_dpc_enqueue(timer->dpc);
		/*
		 * moving ki_wake_waiters out of here, to after the spinlock was
		 * released, should be possibe. but it causes, at least on
		 * aarch64 under kvm, a stale wait block to be attempted to be
		 * woken by ki_wake_waiters. (being stack allocated, it's been
		 * trampled by now)
		 *
		 * it shouldn't be possible, because if ki_signal has queued a
		 * wait block on the waiters_queue, then we should have custody
		 * of that thread - it should not wake until we give the
		 * go-ahead. nevertheless, it happens - it seems somehow the
		 * thread wakes anyway.
		 */
		ki_wake_waiters(&waiters_queue);
		__atomic_store_n(&timer->state, kTimerDisabled,
		    __ATOMIC_RELEASE);
		ke_spinlock_release_nospl(&timer->hdr.spinlock);
	}
}

/*
 * Algorithms owed to Howard Hinnant:
 * https://howardhinnant.github.io/date_algorithms.html
 */
void
civil_from_days(int z, int *year, unsigned int *month, unsigned int *day)
{
	z += 719468;
	const int era = (z >= 0 ? z : z - 146096) / 146097;
	const unsigned doe = (unsigned)(z - era * 146097); // [0, 146096]
	const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) /
	    365; // [0, 399]
	const int y = (int)(yoe) + era * 400;
	const unsigned doy = doe -
	    (365 * yoe + yoe / 4 - yoe / 100);		 // [0, 365]
	const unsigned mp = (5 * doy + 2) / 153;	 // [0, 11]
	const unsigned d = doy - (153 * mp + 2) / 5 + 1; // [1, 31]
	const unsigned m = mp < 10 ? mp + 3 : mp - 9;	 // [1, 12]

	*year = y + (m <= 2);
	*month = m;
	*day = d;
}

unsigned
weekday_from_days(int z)
{
	return (z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

void
ke_format_time(nanosecs_t nanosecs, char *out, size_t size)
{
	static const char *month_names[] = { "JAN", "FEB", "MAR", "APR", "MAY",
		"JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
	uint64_t unix_time = nanosecs / NS_PER_S;
	int days_since_epoch = unix_time / 60 / 60 / 24;
	int year;
	unsigned month, day;

	civil_from_days(days_since_epoch, &year, &month, &day);

	npf_snprintf(out, size,
	    "%02d-%s-%d %02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%2" PRIu64,
	    day, month_names[month - 1], year, unix_time / 60 / 60 % 24,
	    unix_time / 60 % 60, unix_time % 60,
	    (nanosecs % NS_PER_S) / 1000000);
}
