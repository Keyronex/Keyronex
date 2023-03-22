/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */

#include "kdk/kernel.h"
#include "kdk/machdep.h"
#include "kdk/process.h"

#define NANOPRINTF_IMPLEMENTATION
#include <nanoprintf/nanoprintf.h>

kspinlock_t dispatcher_lock = KSPINLOCK_INITIALISER;
kspinlock_t dpc_queues_lock = KSPINLOCK_INITIALISER;
kspinlock_t dprintf_lock = KSPINLOCK_INITIALISER;
kcpu_t cpu_bsp;
kcpu_t **all_cpus;
size_t ncpus;

ipl_t
splraise(ipl_t spl)
{
	ipl_t oldspl = splget();
	kassert(oldspl <= spl);
	if (oldspl < spl)
		splx(spl);
	return oldspl;
}

eprocess_t *
eprocess(kprocess_t *process)
{
	return (eprocess_t *)process;
}

static void
ki_dpc_int_dispatch(void)
{
	ipl_t ipl = spldpc();
	kcpu_t *cpu = hl_curcpu();

	/* FIXME: xxx test/set with ints off, or atomic test/set?  */
	while (cpu->dpc_int) {
		cpu->dpc_int = false;
		while (true) {
			ipl_t hiipl = ke_acquire_dpc_lock();

			kdpc_t *dpc = TAILQ_FIRST(&cpu->dpc_queue);
			if (dpc) {
				TAILQ_REMOVE(&cpu->dpc_queue, dpc, queue_entry);
			}

			ke_release_dpc_lock(hiipl);

			if (dpc == NULL)
				break;

			kassert(dpc->callback != NULL);
			dpc->callback(dpc->arg);
		}
	}

	splx(ipl);
}

void
ki_ipl_lowered(ipl_t from, ipl_t to)
{
	if (to < kIPLDPC && hl_curcpu()->dpc_int) {
		ki_dpc_int_dispatch();

		ipl_t ipl = ke_acquire_dispatcher_lock();
		if (hl_curcpu()->reschedule_reason != kRescheduleReasonNone) {
			ki_reschedule();
			/* ki_reschedule has released the dispatcher lock */
			splx(ipl);
		} else
			ke_release_dispatcher_lock(ipl);
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
		TAILQ_REMOVE(&cpu->runqueue, cand, runqueue_link);

	return cand;
}

/*!
 * @pre dispatcher lock held
 * @post dispatcher lock released, IPL remains at high level
 */
void
ki_reschedule(void)
{
	kcpu_t *cpu = hl_curcpu();
	kthread_t *curthread = ke_curthread(), *next;

	kassert(splget() == kIPLDPC);
	kassert(ke_spinlock_held(&dispatcher_lock));

	if (curthread == cpu->idle_thread) {
		/*! idle thread must never wait, try to exit, whatever */
		kassert(curthread->state == kThreadStateRunning);
		curthread->state = kThreadStateRunnable;
	} else if (curthread->state == kThreadStateRunning) {
		/*! currently running - replace on runqueue */
		curthread->state = kThreadStateRunnable;
		TAILQ_INSERT_TAIL(&cpu->runqueue, curthread, runqueue_link);
	} else if (curthread->state == kThreadStateWaiting) {
		/*! thread wants to go to sleep, don't enqueue on runqueue */
		kdprintf("thread %p going to wait\n", curthread);
		/* accounting? */
	}

	curthread->stats.total_run_time +=
	    MAX2((ke_get_ticks(cpu) - curthread->stats.last_start_time), 1);
	curthread->stats.last_start_time = ke_get_ticks(cpu);

	next = next_thread(cpu);
	next->state = kThreadStateRunning;
	next->timeslice = 5;
	next->stats.last_start_time = ke_get_ticks(cpu);
	cpu->current_thread = next;
	cpu->reschedule_reason = kRescheduleReasonNone;

	if (curthread == next) {
		ke_spinlock_release_nospl(&dispatcher_lock);
		return;
	} else {
		vm_map_activate(eprocess(next->process)->map);
	}

	hl_switch(curthread, next);
}

void
ki_raise_dpc_interrupt(void)
{
	hl_curcpu()->dpc_int = true;
	if (splget() < kIPLDPC)
		ki_dpc_int_dispatch();
}

void
ke_dpc_enqueue(kdpc_t *dpc)
{
	if (splget() < kIPLDPC) {
		ipl_t ipl = spldpc();
		dpc->callback(dpc->arg);
		splx(ipl);
		return;
	}

	ipl_t ipl = ke_acquire_dpc_lock();
	TAILQ_INSERT_TAIL(&hl_curcpu()->dpc_queue, dpc, queue_entry);
	ki_raise_dpc_interrupt();
	ke_release_dpc_lock(ipl);
}
