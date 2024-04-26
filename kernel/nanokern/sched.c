#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "ki.h"

#if SMP == 1
kcpu_t **cpus;
#else
kcpu_t *cpus[1] = { &bootstrap_cpu };
#endif
size_t ncpus;

kspinlock_t scheduler_lock = KSPINLOCK_INITIALISER,
	    done_lock = KSPINLOCK_INITIALISER;
struct kthread_queue ready_queue = TAILQ_HEAD_INITIALIZER(ready_queue),
		     done_queue = TAILQ_HEAD_INITIALIZER(done_queue);

void md_raise_dpc_interrupt(void);
void timer_expiry_dpc(void*);

void
done_thread_dpc(void *)
{
	static kspinlock_t done_lock = KSPINLOCK_INITIALISER;
	ipl_t ipl;
	kthread_t *thread;

	ipl = ke_spinlock_acquire(&done_lock);
	while ((thread = TAILQ_FIRST(&done_queue)) != NULL) {
		TAILQ_REMOVE(&done_queue, thread, queue_link);
		vm_kfree((vaddr_t)thread->kstack_base, KSTACK_SIZE / PGSIZE, 0);
		kmem_free(thread, sizeof(*thread));
	}
	ke_spinlock_release(&done_lock, ipl);
}

void
ki_cpu_init(kcpu_t *cpu, kthread_t *idle_thread)
{
	md_cpu_init(cpu);
	curcpu()->dpc_int = false;
	curcpu()->dpc_lock = (kspinlock_t)KSPINLOCK_INITIALISER;
	TAILQ_INIT(&curcpu()->dpc_queue);
	curcpu()->nanos = 0;
	curcpu()->reschedule_reason = kRescheduleReasonNone;
	curcpu()->idle_thread = idle_thread;
	curcpu()->curthread = idle_thread;
	TAILQ_INIT(&curcpu()->timer_queue);
	curcpu()->done_thread_dpc.cpu = NULL;
	curcpu()->done_thread_dpc.arg = curcpu();
	curcpu()->done_thread_dpc.callback = done_thread_dpc;
	curcpu()->timer_expiry_dpc.cpu = NULL;
	curcpu()->timer_expiry_dpc.arg = curcpu();
	curcpu()->timer_expiry_dpc.callback = timer_expiry_dpc;

	ki_rcu_per_cpu_init(&cpu->rcu_cpustate);
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

	ipl_t ipl = ke_spinlock_acquire_at(&curcpu()->dpc_lock, kIPLHigh);
	if (dpc->cpu == NULL) {
		dpc->cpu = curcpu();
		TAILQ_INSERT_TAIL(&curcpu()->dpc_queue, dpc, queue_entry);
		md_raise_dpc_interrupt();
	}
	ke_spinlock_release(&curcpu()->dpc_lock, ipl);
}

void
ki_dispatch_dpcs(kcpu_t *cpu)
{
	kassert(splget() == kIPLDPC);
	while (cpu->dpc_int) {
		cpu->dpc_int = false;
		while (true) {
			ipl_t ipl;
			kdpc_t *dpc;

			ipl = ke_spinlock_acquire_at(&curcpu()->dpc_lock,
			    kIPLHigh);
			dpc = TAILQ_FIRST(&curcpu()->dpc_queue);
			if (dpc != NULL) {
				TAILQ_REMOVE(&curcpu()->dpc_queue, dpc,
				    queue_entry);
				dpc->cpu = NULL;
			} else {
				ke_spinlock_release(&curcpu()->dpc_lock, ipl);
				break;
			}

			ke_spinlock_release(&curcpu()->dpc_lock, ipl);
			/* Now at IPL=dpc */

			dpc->callback(dpc->arg);
		}

		if (cpu->reschedule_reason != kRescheduleReasonNone) {
			ipl_t ipl = ke_acquire_scheduler_lock();
			ki_reschedule();
			/* IPL remains at dpc but scheduler lock was dropped */
			splx(ipl);
		}
	}
}

static kthread_t *
next_thread(kcpu_t *cpu)
{
	kthread_t *cand;

	cand = TAILQ_FIRST(&ready_queue);
	if (!cand)
		cand = cpu->idle_thread;
	else
		TAILQ_REMOVE(&ready_queue, cand, queue_link);

	return cand;
}

/*!
 *
 * \pre Dispatcher lock held
 * \post Dispatcher lock released
 * \post IPL = dpc
 */
void
ki_reschedule(void)
{
	kcpu_t *cpu = curcpu();
	kthread_t *old_thread = cpu->curthread, *next;
	bool drop = false;

	kassert(splget() == kIPLDPC);
	kassert(ke_spinlock_held(&scheduler_lock));

	if (old_thread == cpu->idle_thread) {
		/*! idle thread must never wait, try to exit, whatever */
		kassert(old_thread->state == kThreadStateRunning);
		old_thread->state = kThreadStateRunnable;
	} else if (old_thread->state == kThreadStateRunning) {
		/*! currently running - replace on runqueue */
		old_thread->state = kThreadStateRunnable;
		TAILQ_INSERT_TAIL(&ready_queue, old_thread, queue_link);
	} else if (old_thread->state == kThreadStateWaiting) {
		/*! thread wants to go to sleep, don't enqueue on runqueue */
#if DEBUG_SCHED == 1
		kprintf("thread %p going to wait\n", old_thread);
#endif
		/* accounting? */
	} else if (old_thread->state == kThreadStateDone) {
		/* exit the thread */
		ke_spinlock_acquire_nospl(&done_lock);
		TAILQ_INSERT_TAIL(&done_queue, old_thread, queue_link);
		ke_dpc_enqueue(&cpu->done_thread_dpc);
		ke_spinlock_release_nospl(&done_lock);
	}

	next = next_thread(cpu);
	next->state = kThreadStateRunning;
	next->timeslice = 5;
	cpu->curthread = next;
	cpu->reschedule_reason = kRescheduleReasonNone;

	(void)drop;
	ki_rcu_quiet();

	if (old_thread == next) {
		ke_spinlock_release_nospl(&scheduler_lock);
		return;
	}

	/* activate VM map... */
	ke_spinlock_release_nospl(&scheduler_lock);
	md_switch(old_thread);
}

void
ki_thread_resume_locked(kthread_t *thread)
{
	kassert(ke_spinlock_held(&scheduler_lock));
	TAILQ_INSERT_HEAD(&ready_queue, thread, queue_link);
	curcpu()->reschedule_reason = kRescheduleReasonPreempted;
	md_raise_dpc_interrupt();
}

void
ke_thread_resume(kthread_t *thread)
{
	ipl_t ipl = ke_acquire_scheduler_lock();
	ki_thread_resume_locked(thread);
	ke_release_scheduler_lock(ipl);
}

void
ki_thread_common_init(kthread_t *thread, kcpu_t *last_cpu, void *proc,
    const char *name)
{
	ipl_t ipl;
	thread->last_cpu = &bootstrap_cpu;
	thread->timeslice = 0;
	thread->name = name;
	thread->timeslice = 5;
	ke_timer_init(&thread->wait_timer);
	thread->wait_result = kKernWaitStatusOK;
	ipl = ke_acquire_scheduler_lock();
	ke_release_scheduler_lock(ipl);
}
