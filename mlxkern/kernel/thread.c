/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 15 2023.
 */

#include "kernel/ke.h"
#include "vm/vm.h"
#include "vm/vmem.h"

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
	kcpu_t		  *cpu;

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
	//kmem_asprintf((char **)&thread->wait_timer.name,
	//    "thread %p wait timeout", thread);
	thread->wait_timer.state = kTimerDisabled;
	ipl_t ipl = ke_spinlock_acquire(&proc->lock);
	SLIST_INSERT_HEAD(&proc->threads, thread, kproc_threads_link);
	ke_spinlock_release(&proc->lock, ipl);
}

void
ke_thread_init(kprocess_t *proc, kthread_t *thread, void (*start_fun)(void *),
    void *start_arg, const char *name)
{
	kcpu_t *cpu = nextcpu();

	ki_thread_common_init(thread, cpu, proc, name);
	thread->state = kThreadStateInitial;
	thread->kstack = vm_kalloc(6, kVMemSleep) + 6 * PGSIZE;

	kmd_thread_init(thread, start_fun, start_arg);
}