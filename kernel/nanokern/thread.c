#include <kern/kmem.h>
#include <md/intr.h>
#include <md/spl.h>

#include <nanokern/kernmisc.h>
#include <nanokern/thread.h>
#include <vm/vm.h>

kspinlock_t nk_lock = KSPINLOCK_INITIALISER;
kspinlock_t callouts_lock = KSPINLOCK_INITIALISER;
kspinlock_t nk_dbg_lock = KSPINLOCK_INITIALISER;
kthread_t   thread0;
kprocess_t  proc0;
kcpu_t	    cpu0;

/*! array of all CPUs */
kcpu_t **all_cpus;
/*! count of CPUs **/
size_t ncpus;
/*! cpu roundrobin for allocating threads to */
size_t lastcpu = 0;

/*! CPU round robin for assigning threads to; should do better some day ofc. */
static kcpu_t *
nextcpu()
{
	static kspinlock_t lock = KSPINLOCK_INITIALISER;
	kcpu_t		  *cpu;

	ipl_t ipl = nk_spinlock_acquire(&lock);
	if (++lastcpu >= ncpus)
		lastcpu = 0;
	cpu = all_cpus[lastcpu];
	nk_spinlock_release(&lock, ipl);

	return cpu;
}

void
nkx_thread_common_init(kthread_t *thread, kcpu_t *cpu, kprocess_t *proc)
{
	thread->cpu = cpu;
	thread->process = proc;
	thread->saved_ipl = kSPL0;
	kmem_asprintf((char**)&thread->wait_callout.name, "thread %p wait timeout",
	    thread);
	thread->wait_callout.state = kCalloutDisabled;
	ipl_t ipl = nk_spinlock_acquire(&proc->lock);
	SLIST_INSERT_HEAD(&proc->threads, thread, proc_link);
	nk_spinlock_release(&proc->lock, ipl);
}

void
nk_thread_init(kprocess_t *proc, kthread_t *thread, void (*start_fun)(void *),
    void *start_arg)
{
	kcpu_t *cpu = nextcpu();

	nkx_thread_common_init(thread, cpu, proc);
	thread->state = kThreadStateSuspended;
	thread->kstack = vm_kalloc(4, kVMKSleep) + 4 * PGSIZE;

	md_thread_init(thread, start_fun, start_arg);
}

void
nk_thread_resume(kthread_t *thread)
{
	nk_assert(thread->state == kThreadStateSuspended);
	ipl_t ipl = nk_spinlock_acquire(&thread->cpu->sched_lock);

	TAILQ_INSERT_HEAD(&thread->cpu->runqueue, thread, queue_link);

	if (thread->cpu == curcpu()) {
		curcpu()->running_thread->timeslice = 0;
		nk_raise_dispatch_interrupt();
	} else {
		md_ipi_reschedule(thread->cpu);
	}

	nk_spinlock_release(&thread->cpu->sched_lock, ipl);
}
