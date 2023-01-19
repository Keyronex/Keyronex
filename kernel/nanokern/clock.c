#include <sys/param.h>

#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>

#include "md/intr.h"
#include "md/spl.h"

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

static void
do_reschedule(void)
{
	/* time to reschedule */
	kcpu_t	  *cpu = curcpu();
	ipl_t	   ipl = spldispatch();
	bool	   iff;
	kthread_t *cur, *next;

	/* switch can only occur at IPL dispatch (for now */
	nk_assert(ipl == kSPL0);

	nk_spinlock_acquire(&cpu->sched_lock);
	cur = cpu->running_thread;

	if (cur == cpu->idle_thread) {
		/*! idle thread must never wait, try to exit, whatever */
		nk_assert(cur->state == kThreadStateRunning);
	}

	if (cur->state == kThreadStateRunning) {
		cur->state = kThreadStateRunnable;
	}

	next = next_thread(cpu);

	nk_dbg("Cur  = %p, next = %p\n", cur, next);

	/* start timeslicing if needed ... */
	// setup timeslicer callout

	if (next == cur) {
		nk_spinlock_release_nospl(&cpu->sched_lock);
		splx(ipl);
		return;
	}

	/* disable interrupts so we can return to IPL 0 */
	iff = md_intr_disable();
	/* FIXME: xxx we lower and potentially risk more DPCs dispatched! */
	splx(ipl);

	/* do the switch. It will release the CPU lock at an appropriate time */
	md_switch(cur, next);

	/* back from the switch. Re-enable interrupts and return. */
	md_intr_x(iff);
}

static void
do_soft_int_dispatch()
{
	while (true) {
		ipl_t ipl = splhigh();

		kdpc_t *dpc = TAILQ_FIRST(&curcpu()->dpc_queue);
		if (dpc)
			TAILQ_REMOVE(&curcpu()->dpc_queue, dpc, queue_entry);
		splx(ipl);

		if (dpc == NULL)
			break;

		dpc->callback(dpc->arg);
	}

	do_reschedule();
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
	if (to < kSPLDispatch) {
		/* FIXME: xxx test/set with ints off, or atomic test/set?  */
		while (curcpu()->soft_int_dispatch) {
			curcpu()->soft_int_dispatch = 0;
			do_soft_int_dispatch();
		}
	}
}

void
nk_raise_dispatch_interrupt(void)
{
	if (splget() < kSPLDispatch)
		do_soft_int_dispatch();
	else
		curcpu()->soft_int_dispatch = true;
}

void
nkx_callout_enqueue(kxcallout_t *callout)
{
	__auto_type queue = &curcpu()->callout_queue;
	kxcallout_t *co;
	ipl_t	     ipl;

#if DEBUG_TIMERS == 1
	kprintf("Enqueuing callout %p\n", callout);
#endif

	nk_assert(callout->nanosecs > 0);
	ipl = splhigh();
	co = TAILQ_FIRST(queue);

	if (!co) {
		TAILQ_INSERT_HEAD(queue, callout, queue_entry);
		md_timer_set(callout->nanosecs);
		goto next;
	} else {
		uint64_t remains = md_timer_get_remaining();
		/* XXX: at least on QEMU, often reading current count > initial
		   count! */
		nk_assert(remains < co->nanosecs);
		co->nanosecs = MIN(remains, co->nanosecs);
	}

	nk_assert(co->nanosecs > 0);

	if (co->nanosecs > callout->nanosecs) {
		co->nanosecs -= callout->nanosecs;
		TAILQ_INSERT_HEAD(queue, callout, queue_entry);
		md_timer_set(callout->nanosecs);
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
	splx(ipl);
}

void
nkx_callout_dequeue(kxcallout_t *callout)
{
	__auto_type queue = &curcpu()->callout_queue;
	kxcallout_t *co;
	bool	     iff = md_intr_disable();

	// TODO(med): can have false wakeups if an interrupt is pending?

	nk_assert(callout->state == kCalloutPending);

	co = TAILQ_FIRST(queue);

	if (co != callout) {
		TAILQ_REMOVE(queue, callout, queue_entry);
	} else {
		uint64_t remains = md_timer_get_remaining();
		TAILQ_REMOVE(queue, callout, queue_entry);
		/* XXX: at least on QEMU, often reading current count > initial
		   count! */
		// assert(remains < co->nanosecs);
		callout->state = kCalloutDisabled;
		co = TAILQ_FIRST(queue);
		if (co) {
			co->nanosecs += MIN(remains, co->nanosecs);
			md_timer_set(co->nanosecs);
		} else
			/* nothing upcoming */
			md_timer_set(0);
	}

	md_intr_x(iff);
}

void
nkx_cpu_hardclock(md_intr_frame_t *frame, void *arg)
{
	__auto_type queue = &curcpu()->callout_queue;
	kxcallout_t *co;

	co = TAILQ_FIRST(queue);

	nk_assert(co != NULL);

	if (co == NULL)
		return;

	TAILQ_REMOVE(queue, co, queue_entry);
	co->state = kCalloutElapsed;

	nk_dpc_enqueue(&co->dpc);

	/* now set up the next in sequence */
	co = TAILQ_FIRST(queue);
	if (co != NULL)
		md_timer_set(co->nanosecs);
}

void
nkx_reschedule_ipi(md_intr_frame_t *frame, void *arg)
{
	nk_dbg("Reschedule\n");
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
	curcpu()->soft_int_dispatch = true;
	splx(ipl);
}
