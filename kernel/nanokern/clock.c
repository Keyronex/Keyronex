#include <sys/param.h>

#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>

#include "md/intr.h"
#include "md/spl.h"

void
nkx_preempt_dpc(void *arg)
{
	if (!curcpu()->entering_scheduler) {
		curcpu()->entering_scheduler = true;
		nk_raise_dispatch_interrupt();
	}
}

static kthread_t *
next_thread(kcpu_t *cpu)
{
	kthread_t *cand;

	cand = TAILQ_FIRST(&cpu->runqueue);
	if (!cand)
		cand = cpu->idle_thread;
	else
		TAILQ_REMOVE(&cpu->runqueue, cand, queue_link);

	return cand;
}

/* entered at ipl = dispatch */
void
nkx_do_reschedule(ipl_t ipl)
{
	/* time to reschedule */
	kcpu_t	  *cpu = curcpu();
	bool	   iff;
	kthread_t *cur, *next;

#if DEBUG_SCHED == 1
	nk_dbg("\n\nScheduler Entry on CPU %d\n", cpu->num);
#endif

	nk_assert(splget() == kSPLDispatch);

	nk_spinlock_acquire(&cpu->sched_lock);
	cur = cpu->running_thread;

	if (cur == cpu->idle_thread) {
		/*! idle thread must never wait, try to exit, whatever */
		nk_assert(cur->state == kThreadStateRunning);
	} else if (cur->state == kThreadStateRunning) {
		cur->state = kThreadStateRunnable;
		TAILQ_INSERT_TAIL(&cpu->runqueue, cur, queue_link);
	} else if (cur->state == kThreadStateWaiting) {
#if DEBUG_SCHED == 1
		nk_dbg("thread %p going to wait\n", cur);
#endif
		/* accounting? */
	}

	next = next_thread(cpu);
	next->state = kThreadStateRunning;

#if DEBUG_SCHED == 1
	nk_dbg("Current thread = %p, next = %p\n", cur, next);
#endif

	if (next != cpu->idle_thread) {
		next->timeslice = 5;
	} else {
		next->timeslice = INT64_MAX;
	}
	cpu->running_thread = next;
	cpu->entering_scheduler = false;

	if (next == cur) {
		nk_spinlock_release_nospl(&cpu->sched_lock);
		/* FIXME: xxx? see below */
		splx(ipl);
		return;
	}

	/* disable interrupts for safety */
	iff = md_intr_disable();

	/* do the switch. It will release the CPU lock at an appropriate time
	 * and lower IPL */
	md_switch(ipl, cur, next);

	/* back from the switch. Re-enable interrupts and return. */
	splx(ipl);
	md_intr_x(iff);
}

static void
do_soft_int_dispatch()
{
	ipl_t ipl = spldispatch();

	/* FIXME: xxx test/set with ints off, or atomic test/set?  */
	while (curcpu()->soft_int_dispatch) {
		curcpu()->soft_int_dispatch = 0;
		while (true) {
			ipl_t ipl = splhigh();
			/* needed? */
			nk_spinlock_acquire_nospl(&callouts_lock);

			kdpc_t *dpc = TAILQ_FIRST(&curcpu()->dpc_queue);
			if (dpc) {
				dpc->bound = false;
				TAILQ_REMOVE(&curcpu()->dpc_queue, dpc,
				    queue_entry);
			}

			nk_spinlock_release_nospl(&callouts_lock);
			splx(ipl);

			if (dpc == NULL)
				break;

			nk_assert(dpc->callback != NULL);
			dpc->callback(dpc->arg);
		}
	}

	/* do_reschedule drops ipl */
	if (curcpu()->running_thread->timeslice <= 0)
		nkx_do_reschedule(ipl);

	splx(ipl);
}

ipl_t
splraise(ipl_t spl)
{
	ipl_t oldspl = splget();
	nk_assert(oldspl <= spl);
	if (oldspl < spl)
		splx(spl);
	return oldspl;
}

void
nkx_spl_lowered(ipl_t from, ipl_t to)
{
	if (to < kSPLDispatch && curcpu()->soft_int_dispatch) {
		do_soft_int_dispatch();
	}
}

void
nk_raise_dispatch_interrupt(void)
{
	curcpu()->soft_int_dispatch = true;
	if (splget() < kSPLDispatch)
		do_soft_int_dispatch();
}

#define DEBUG_TIMERS 0

void
nkx_callout_enqueue(kxcallout_t *callout)
{
	struct kxcallout_queue *queue;
	kxcallout_t	       *co;
	kcpu_t		       *cpu = curcpu();
	ipl_t			ipl;

#if DEBUG_TIMERS == 1
	nk_dbg(" -- callouts: Enqueuing %s for %lu ns\n", callout->name,
	    callout->nanosecs);
#endif

	ipl = splhigh();
	nk_spinlock_acquire_nospl(&callouts_lock);

	nk_assert(callout->state == kCalloutDisabled);
	nk_assert(callout->nanosecs > 0);
	callout->deadline = callout->nanosecs + cpu->ticks;
	queue = &cpu->callout_queue;

	TAILQ_FOREACH (co, queue, queue_entry) {
		if (co->deadline > callout->deadline) {
			TAILQ_INSERT_BEFORE(co, callout, queue_entry);
			goto next;
		}
	}
	/* no callouts or it's the longest till elapsing */
	TAILQ_INSERT_TAIL(queue, callout, queue_entry);

next:
#if 0
	nk_dbg("---------timer list-----------\n") uint64_t cum = 0;
	TAILQ_FOREACH (co, &cpu->callout_queue, queue_entry) {
		cum += co->nanosecs;
		nk_dbg("%s (%lumillis/cum %lumillis)\n", co->name,
		    co->nanosecs / (NS_PER_S / 1000), cum / (NS_PER_S / 1000));
	}
	nk_dbg("---------ends-----------\n")
#endif
	callout->state = kCalloutPending;
	callout->cpu = curcpu();
	nk_spinlock_release_nospl(&callouts_lock);
	splx(ipl);
}

void
nkx_callout_dequeue(kxcallout_t *callout)
{
	struct kxcallout_queue *queue;
	kcpu_t		       *cpu;
	bool			iff = md_intr_disable();

	nk_spinlock_acquire_nospl(&callouts_lock);

#if DEBUG_TIMERS == 1
	nk_dbg(" -- removing callout %s\n", callout->name);
#endif

	nk_assert(callout->state == kCalloutPending);
	cpu = callout->cpu;
	queue = &cpu->callout_queue;

	callout->state = kCalloutDisabled;

	TAILQ_REMOVE(queue, callout, queue_entry);

	nk_spinlock_release_nospl(&callouts_lock);
	md_intr_x(iff);
}

bool
nkx_cpu_hardclock(md_intr_frame_t *frame, void *arg)
{
	__auto_type queue = &curcpu()->callout_queue;
	kxcallout_t *co;

	uint64_t ticks = atomic_fetch_add(&curcpu()->ticks, NS_PER_S / KERN_HZ);

	/* we are already at spl high */
	nk_spinlock_acquire_nospl(&callouts_lock);

	if (curcpu()->running_thread->timeslice-- <= 0 &&
	    !curcpu()->entering_scheduler) {
		nk_dpc_enqueue(&curcpu()->preempt_dpc);
	}

	while (true) {
		co = TAILQ_FIRST(queue);

		if (co == NULL || co->deadline > ticks)
			goto next;

#if DEBUG_TIMERS == 1
		nk_dbg(" -- callouts: callout %s happened\n", co->name);
#endif

		TAILQ_REMOVE(queue, co, queue_entry);
		/* ! do we want kCalloutElapsed? Do we need it? */
		co->state = kCalloutDisabled;

		nk_dpc_enqueue(&co->dpc);
	}

next:
	nk_spinlock_release_nospl(&callouts_lock);

	return true;
}

bool
nkx_reschedule_ipi(md_intr_frame_t *frame, void *arg)
{
#if DEBUG_SCHED == 1
	nk_dbg("Reschedule IPI received on cpu %lu\n", cpu->num);
#endif
	curcpu()->running_thread->timeslice = 0;
	nk_raise_dispatch_interrupt();
	return true;
}

void
nk_dpc_enqueue(kdpc_t *dpc)
{
	if (splget() < kSPLDispatch) {
		/* TODO: xxx should we spldispatch() here? */
		return dpc->callback(dpc->arg);
	}

	ipl_t ipl = splhigh();
	TAILQ_INSERT_TAIL(&curcpu()->dpc_queue, dpc, queue_entry);
	dpc->bound = true;
	curcpu()->soft_int_dispatch = true;
	splx(ipl);
}
