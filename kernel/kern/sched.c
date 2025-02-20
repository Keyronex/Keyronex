#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "kdk/object.h"
#include "kdk/queue.h"
#include "kdk/vm.h"
#include "ki.h"

kcpu_t **cpus;
size_t ncpus;

uintptr_t idle_mask = 1;
kspinlock_t done_lock = KSPINLOCK_INITIALISER;
struct kthread_queue done_queue = TAILQ_HEAD_INITIALIZER(done_queue);

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
		ke_spinlock_release(&done_lock, ipl);
		obj_release(thread);
		ipl = ke_spinlock_acquire(&done_lock);
	}
	ke_spinlock_release(&done_lock, ipl);
}

void
ki_cpu_init(kcpu_t *cpu, kthread_t *idle_thread)
{
	md_cpu_init(cpu);
	cpu->dpc_lock = (kspinlock_t)KSPINLOCK_INITIALISER;
	TAILQ_INIT(&cpu->dpc_queue);
	cpu->nanos = 0;
	cpu->reschedule_reason = kRescheduleReasonNone;
	cpu->idle_thread = idle_thread;
	cpu->curthread = idle_thread;
	ke_spinlock_init(&cpu->sched_lock);
	TAILQ_INIT(&cpu->timer_queue);
	TAILQ_INIT(&cpu->runqueue);
	cpu->done_thread_dpc.cpu = NULL;
	cpu->done_thread_dpc.arg = cpu;
	cpu->done_thread_dpc.callback = done_thread_dpc;
	cpu->timer_expiry_dpc.cpu = NULL;
	cpu->timer_expiry_dpc.arg = cpu;
	cpu->timer_expiry_dpc.callback = timer_expiry_dpc;
	cpu->local_data->cpu = cpu;
	cpu->local_data->curthread = idle_thread;
#if defined(__amd64__)
	/* move me */
	cpu->local_data->md.soft_ipl = 0;
	cpu->local_data->md.hard_ipl = 0;
	cpu->local_data->md.self = cpu->local_data;
#endif
	ki_rcu_per_cpu_init(&cpu->rcu_cpustate);
}

void
ke_dpc_init(kdpc_t *dpc, void (*callback)(void *), void *arg)
{
	dpc->callback = callback;
	dpc->arg = arg;
	dpc->cpu = NULL;
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
	kassert((uintptr_t)dpc >= 0x10000ull);
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
	while (true) {
		ipl_t ipl;
		kdpc_t *dpc;

		ipl = ke_spinlock_acquire_at(&curcpu()->dpc_lock, kIPLHigh);
		dpc = TAILQ_FIRST(&curcpu()->dpc_queue);
		if (dpc != NULL) {
			TAILQ_REMOVE(&curcpu()->dpc_queue, dpc, queue_entry);
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
		ipl_t ipl = ke_spinlock_acquire(&curthread()->lock);
		ki_reschedule();
		/* IPL remains at dpc but old thread lock was dropped */
		splx(ipl);
		/* curcpu() can have changed, if rescheduled. */
		cpu = curcpu();
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
	kassert(ke_spinlock_held(&old_thread->lock));

	ke_spinlock_acquire_nospl(&cpu->sched_lock);

	if (old_thread == cpu->idle_thread) {
		/*! idle thread must never wait, try to exit, whatever */
		kassert(old_thread->state == kThreadStateRunning);
	} else if (old_thread->state == kThreadStateRunning) {
		/*! currently running - replace on runqueue */
		old_thread->state = kThreadStateRunnable;
		TAILQ_INSERT_TAIL(&cpu->runqueue, old_thread, queue_link);
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
	next->last_cpu = cpu;
	cpu->curthread = next;
	KCPU_LOCAL_STORE(curthread, next);
	cpu->reschedule_reason = kRescheduleReasonNone;
	cpu->old_thread = old_thread;

	ke_spinlock_release_nospl(&cpu->sched_lock);

	(void)drop;
	ki_rcu_quiet();

	if (old_thread == next) {
		ke_spinlock_release_nospl(&old_thread->lock);
		return;
	}

	if (next == cpu->idle_thread) {
		__atomic_fetch_or(&idle_mask, (1 << cpu->num), __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_and(&idle_mask, ~(1 << cpu->num), __ATOMIC_RELAXED);
	}

	/* do the machine-specific switching of state */
	md_switch(old_thread, next);

	/* by this point, curcpu() can have changed. */

	/* we are being returned to - drop the old thread lock */
	ke_spinlock_release_nospl(&curcpu()->old_thread->lock);
}

void
ki_thread_resume_locked(kthread_t *thread)
{
#if SMP
	kcpu_t *chosen = NULL;
	uintptr_t idle = __atomic_load_n(&idle_mask, __ATOMIC_RELAXED);

	kassert(splget() == kIPLDPC);

	if (idle & (1 << curcpu()->num)) {
		chosen = curcpu();
	} else if (idle != 0) {
		chosen = cpus[__builtin_ctz(idle)];
	} else {
		chosen = curcpu();
	}
#else
	kcpu_t *chosen = curcpu();
#endif

	ke_spinlock_acquire_nospl(&chosen->sched_lock);
	TAILQ_INSERT_HEAD(&chosen->runqueue, thread, queue_link);
	chosen->reschedule_reason = kRescheduleReasonPreempted;
	ke_spinlock_release_nospl(&chosen->sched_lock);

#if SMP
	if (chosen == curcpu())
		md_raise_dpc_interrupt();
	else
		md_send_dpc_ipi(chosen);
#else
	md_raise_dpc_interrupt();
#endif
}

void
ke_thread_resume(kthread_t *thread)
{
	ipl_t ipl = ke_spinlock_acquire(&thread->lock);
	ki_thread_resume_locked(thread);
	ke_spinlock_release(&thread->lock, ipl);
}

void
ki_thread_common_init(kthread_t *thread, kcpu_t *last_cpu, kprocess_t *proc,
    const char *name)
{
	ipl_t ipl;
	ke_spinlock_init(&thread->lock);
	thread->user = false;
	thread->last_cpu = last_cpu;
	thread->timeslice = 0;
	thread->name = name;
	thread->timeslice = 5;
	thread->process = proc;
	thread->port = NULL;
	thread->port_msg = NULL;
	thread->in_trap_recoverable = false;
	thread->tcb = 0;
	ke_timer_init(&thread->wait_timer);
	ipl = ke_spinlock_acquire(&proc->lock);
	proc->thread_count++;
	LIST_INSERT_HEAD(&proc->thread_list, thread, list_link);
	ke_spinlock_release(&proc->lock, ipl);
}

void
ke_thread_deinit(kthread_t *thread)
{
	kprocess_t *proc = thread->process;
	ipl_t ipl;

	ipl = ke_spinlock_acquire(&proc->lock);
	kassert(thread->state == kThreadStateDone);
	/* unlink from process thread list */
	LIST_REMOVE(thread, list_link);
	if (--proc->thread_count == 0)
		proc->state = kProcessStateTerminated;
	ke_spinlock_release(&proc->lock, ipl);
}

void
ke_process_init(kprocess_t *proc)
{
	ke_spinlock_init(&proc->lock);
	LIST_INIT(&proc->thread_list);
	proc->thread_count = 0;
	proc->state = kProcessStateLive;
}

ktrap_recovery_frame_t *
ke_trap_recovery_begin(void)
{
	kthread_t *thread = curthread();
	thread->in_trap_recoverable = true;
	return &thread->trap_recovery;
}

void
ke_trap_recovery_end(void)
{
	curthread()->in_trap_recoverable = false;
}
