#include <sys/param.h>

#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>

#include "md/intr.h"
#include "md/spl.h"

static void
preempt_dpc(void *arg)
{
	/* xxx do we really need this? we are already servicing the soft int*/
	nk_raise_dispatch_interrupt();
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
	nk_dbg("\n\nScheduler Entry\n");
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

	/* start timeslicing if needed ... */
	if (next != cpu->idle_thread &&
	    cpu->preempt_callout.state != kCalloutPending) {
		cpu->preempt_callout.nanosecs = NS_PER_S / 20;
		cpu->preempt_callout.dpc.callback = preempt_dpc;
		nkx_callout_enqueue(&cpu->preempt_callout);
	}

	if (next == cur) {
		nk_spinlock_release_nospl(&cpu->sched_lock);
		/* FIXME: xxx? see below */
		splx(ipl);
		return;
	}

	/* disable interrupts so we can return to IPL 0 */
	iff = md_intr_disable();
	/* FIXME: xxx we lower and potentially risk more DPCs dispatched! */
	/*  - this indeed proved to be a problem, SPL lowering moved to the
	 * switcher
	 *
	 * i am now concerned that, when we replace md_switch() with a sane
	 * implementation, that we might miss software interrupts happening
	 * between end of DPC processing and beginning of rescheduling.
	 *
	 * a potential solution: disable interrupts prior to doing a check on
	 * pending DPCs. if there are none more, then restore prior interrupt
	 * state and return, lowering spl on close. this should work because
	 * only an interrupt on a core (or indeed a manual request) can raise a
	 * a software interrupt. and so with interrupts blocked, we are not
	 * going to see one raised until we iret into the next thread..
	 *
	 * we might also improve things by deferring the actual lowering of SPL
	 * below dispatch until we are out of that codepath?
	 *
	 * finally, should we save SPL in the thread on sleep? i think it might
	 * be important for the case of waiting?
	 */
	// splx(ipl);

	/* do the switch. It will release the CPU lock at an appropriate time
	 * and lower IPL */
	md_switch(ipl, cur, next);

	/* back from the switch. Re-enable interrupts and return. */
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

			dpc->callback(dpc->arg);
		}
	}

	/* do_reschedule drops ipl */
	nkx_do_reschedule(ipl);
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

void
nkx_callout_enqueue(kxcallout_t *callout)
{
	struct kxcallout_queue *queue;
	kxcallout_t	       *co;
	kcpu_t		       *cpu = curcpu();
	ipl_t			ipl;

#if DEBUG_TIMERS == 1
	nk_dbg("Enqueuing callout %p\n", callout);
#endif

	ipl = splhigh();
	nk_spinlock_acquire_nospl(&callouts_lock);

	nk_assert(callout->state == kCalloutDisabled);
	nk_assert(callout->nanosecs > 0);
	queue = &cpu->callout_queue;
	co = TAILQ_FIRST(queue);

	if (!co) {
		TAILQ_INSERT_HEAD(queue, callout, queue_entry);
		md_timer_set(cpu, callout->nanosecs);
		goto next;
	} else {
		uint64_t remains = md_timer_get_remaining(cpu);
		/* XXX: at least on QEMU, often reading current count > initial
		   count! */
		nk_assert(remains < co->nanosecs);
		co->nanosecs = MIN(remains, co->nanosecs);
	}

	nk_assert(co->nanosecs > 0);

	if (co->nanosecs > callout->nanosecs) {
		co->nanosecs -= callout->nanosecs;
		TAILQ_INSERT_HEAD(queue, callout, queue_entry);
		md_timer_set(cpu, callout->nanosecs);
		goto next;
	}

	while (co->nanosecs < callout->nanosecs) {
		kxcallout_t *next;
		callout->nanosecs -= co->nanosecs;
		next = TAILQ_NEXT(co, queue_entry);
		if (next == NULL)
			break;
		co = next;
	}

	TAILQ_INSERT_AFTER(queue, co, callout, queue_entry);

next:
	callout->state = kCalloutPending;
	callout->cpu = curcpu();
	nk_spinlock_release_nospl(&callouts_lock);
	splx(ipl);
}

void
nkx_callout_dequeue(kxcallout_t *callout)
{
	struct kxcallout_queue *queue;
	kxcallout_t	       *co;
	kcpu_t		       *cpu;
	bool			iff = md_intr_disable();

	nk_spinlock_acquire_nospl(&callouts_lock);

	nk_assert(callout->state == kCalloutPending);
	cpu = callout->cpu;
	queue = &cpu->callout_queue;

	// TODO(med): can have false wakeups if an interrupt is pending?
	// need to maybe do timekeeping in hardclock or something

	nk_assert(callout->state == kCalloutPending);

	co = TAILQ_FIRST(queue);

	if (co != callout) {
		TAILQ_REMOVE(queue, callout, queue_entry);
	} else {
		uint64_t remains = md_timer_get_remaining(cpu);
		TAILQ_REMOVE(queue, callout, queue_entry);
		/* XXX: at least on QEMU, often reading current count > initial
		   count! */
		// assert(remains < co->nanosecs);
		callout->state = kCalloutDisabled;
		co = TAILQ_FIRST(queue);
		if (co) {
			co->nanosecs += MIN(remains, co->nanosecs);
			md_timer_set(cpu, co->nanosecs);
		} else
			/* nothing upcoming */
			md_timer_set(cpu, 0);
	}

	nk_spinlock_release_nospl(&callouts_lock);
	md_intr_x(iff);
}

void
nkx_cpu_hardclock(md_intr_frame_t *frame, void *arg)
{
	__auto_type queue = &curcpu()->callout_queue;
	kxcallout_t *co;

	co = TAILQ_FIRST(queue);

#if DEBUG_TIMERS == 1
	nk_dbg("callout %p happened\n", co);
#endif

	nk_assert(co != NULL);

	if (co == NULL)
		return;

	nk_spinlock_acquire_nospl(&callouts_lock);

	TAILQ_REMOVE(queue, co, queue_entry);
	/* ! do we want kCalloutElapsed? Do we need it? */
	co->state = kCalloutDisabled;

	nk_dpc_enqueue(&co->dpc);

	/* now set up the next in sequence */
	co = TAILQ_FIRST(queue);
	if (co != NULL)
		md_timer_set(curcpu(), co->nanosecs);

	nk_spinlock_release_nospl(&callouts_lock);
}

void
nkx_reschedule_ipi(md_intr_frame_t *frame, void *arg)
{
#if DEBUG_SCHED == 1
	nk_dbg("Reschedule IPI received on cpu %lu\n", cpu->num);
#endif
	nk_raise_dispatch_interrupt();
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
