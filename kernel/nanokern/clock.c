#include "kdk/nanokern.h"
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
ki_timer_enqueue(ktimer_t *callout)
{
	ktimer_t *existing;
	kcpu_t *cpu;
	ipl_t ipl;

	ipl = ke_spinlock_acquire_at(&curcpu()->dpc_lock, kIPLHigh);
	cpu = curcpu();

	kassert(callout->pending == false);
	kassert(callout->dpc.cpu == NULL);
	kassert(callout->nanosecs > 0);
	callout->deadline = callout->nanosecs + cpu->nanos;

	TAILQ_FOREACH (existing, &cpu->timer_queue, queue_entry) {
		if (existing->deadline > callout->deadline) {
			TAILQ_INSERT_BEFORE(existing, callout, queue_entry);
			goto next;
		}
	}
	/* no callouts or it's the longest till elapsing */
	TAILQ_INSERT_TAIL(&cpu->timer_queue, callout, queue_entry);

next:
	callout->pending = true;
	callout->cpu = cpu;

	ke_spinlock_release(&cpu->dpc_lock, ipl);
}

void
ki_timer_dequeue(ktimer_t *callout)
{
	kcpu_t *cpu;
	ipl_t ipl;

	ipl = ke_spinlock_acquire_at(&callout->cpu->dpc_lock, kIPLHigh);
	cpu = callout->cpu;

	kassert(cpu != NULL);
	kassert(callout->pending);

	callout->pending = false;
	TAILQ_REMOVE(&cpu->timer_queue, callout, queue_entry);

	ke_spinlock_release(&callout->cpu->dpc_lock, ipl);
}

bool
ki_cpu_hardclock(md_intr_frame_t *frame, void *arg)
{
	ktimer_t *co;
	nanosecs_t nanos;
	kcpu_t *cpu;
	ipl_t ipl;

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

	while (true) {
		co = TAILQ_FIRST(&cpu->timer_queue);

		if (co == NULL || co->deadline > nanos)
			goto next;

		TAILQ_REMOVE(&cpu->timer_queue, co, queue_entry);
		/* ! do we want kCalloutElapsed? Do we need it? */
		co->pending = false;

		TAILQ_INSERT_TAIL(&cpu->dpc_queue, &co->dpc, queue_entry);
		co->dpc.cpu = cpu;
	}

next:
	ke_spinlock_release(&cpu->dpc_lock, ipl);

	return true;
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
