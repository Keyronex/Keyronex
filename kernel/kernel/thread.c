/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Feb 15 2023.
 */

#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "ke_internal.h"

/*! cpu roundrobin for allocating threads to */
size_t lastcpu = 0;

/*!
 * this should really be a more intelligent algorithm, unless we get thread
 * migration
 */
static kcpu_t *
nextcpu()
{
	static kspinlock_t lock = KSPINLOCK_INITIALISER;
	kcpu_t *cpu;

	ipl_t ipl = ke_spinlock_acquire(&lock);
	if (++lastcpu >= ncpus)
		lastcpu = 0;
	cpu = all_cpus[lastcpu];
	ke_spinlock_release(&lock, ipl);

	return cpu;
}

void
ki_thread_start(kthread_t *thread)
{
	ipl_t ipl = ke_acquire_dispatcher_lock();

	TAILQ_INSERT_HEAD(&thread->cpu->runqueue, thread, runqueue_link);

	if (thread->cpu == hl_curcpu()) {
		hl_curcpu()->reschedule_reason = kRescheduleReasonPreempted;
		ki_raise_dpc_interrupt();
	} else {
		hl_ipi_reschedule(thread->cpu);
	}

	ke_release_dispatcher_lock(ipl);
}

void
ki_thread_common_init(kthread_t *thread, kcpu_t *cpu, kprocess_t *proc,
    const char *name)
{
	thread->cpu = cpu;
	thread->process = proc;
	thread->saved_ipl = kIPL0;
	thread->name = name;
	thread->timeslice = 5;
	// kmem_asprintf((char **)&thread->wait_timer.name,
	//     "thread %p wait timeout", thread);
	thread->wait_timer.state = kTimerDisabled;
	ipl_t ipl = ke_spinlock_acquire(&proc->lock);
	SLIST_INSERT_HEAD(&proc->threads, thread, kproc_threads_link);
	ke_spinlock_release(&proc->lock, ipl);
}

int
ki_thread_init(kthread_t *thread, kprocess_t *proc, const char *name,
    void (*start)(void *), void *arg)
{
	kcpu_t *cpu = nextcpu();

	ki_thread_common_init(thread, cpu, proc, name);
	thread->state = kThreadStateInitial;
	thread->kstack = vm_kalloc(6, kVMemSleep) + 6 * PGSIZE;

	kmd_thread_init(thread, start, arg);

	return 0;
}

int ke_process_init(kprocess_t *kproc) {
	SLIST_INIT(&kproc->threads);
	ke_spinlock_init(&kproc->lock);
	return 0;
}

void
dbg_dump_threads(void)
{
	kthread_t *thr;
	SLIST_FOREACH (thr, &kernel_process.kproc.threads, kproc_threads_link) {
		kdprintf("thread %s <%p>: ", thr->name, thr);
		if (thr->state == kThreadStateWaiting) {
			kdprintf("waiting (%s)", thr->wait_reason);
		} else
			kdprintf("state: %d", thr->state);
		kdprintf("\n");
	}
}

void
dbg_trace_threads(void)
{
	kthread_t *thr;
	SLIST_FOREACH (thr, &kernel_process.kproc.threads, kproc_threads_link) {
		kdprintf("thread %s <%p>: \n", thr->name, thr);
		md_intr_frame_trace(&thr->frame);
	}
}
