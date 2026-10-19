/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file thread.c
 * @brief Kernel threads.
 */

#include <sys/cpulocal.h>
#include <sys/k_intr.h>
#include <sys/k_log.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>

#include <libkern/idalloc.h>
#include <sched.h>

uint8_t tid_bitmap[UINT16_MAX / 8];
struct id_allocator tid_allocator = IDALLOC_INITIALISER(tid_bitmap, UINT16_MAX);
static kspinlock_t retire_lock = KSPINLOCK_INITIALISER;
static TAILQ_HEAD(, kthread) retire_queue = TAILQ_HEAD_INITIALIZER(retire_queue);
kevent_t ke_thread_retire_event;

void
kep_thread_global_init(void)
{
	ke_event_init(&ke_thread_retire_event, false);
}

void
kep_thread_switch_finish(void)
{
	kthread_t *prev = CPU_LOCAL_LOAD(prevthread);
	bool terminated = prev->state == TS_TERMINATED;

	/*
	 * Called after a context switch has finished.
	 *
	 * The principle of this is that the old thread's lock stays held across
	 * the context switch. ke_thread_resume() interlocks with us by taking
	 * the thread lock. This prevents anyone else running oldt until it is
	 * definitively quiescent.
	 *
	 * (The arch thread trampolines do the same for newly-created threads.)
	 */

	kassert(ke_ipl() == IPL_DISP);

	ke_spinlock_exit_nospl(&prev->lock);

	if (terminated) {
		/* terminated thread is now in our custody, queue for retire */
		ke_spinlock_enter_nospl(&retire_lock);
		TAILQ_INSERT_TAIL(&retire_queue, prev, tqlink);
		ke_spinlock_exit_nospl(&retire_lock);
		ke_event_set_signalled(&ke_thread_retire_event, true);
	}
}

kthread_t *
ke_thread_retire_dequeue(void)
{
	kthread_t *thread;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&retire_lock);
	thread = TAILQ_FIRST(&retire_queue);
	if (thread != NULL)
		TAILQ_REMOVE(&retire_queue, thread, tqlink);
	ke_spinlock_exit(&retire_lock, ipl);

	return thread;
}

void
ke_thread_deinit(kthread_t *thread)
{
	kassert(thread->state == TS_TERMINATED || thread->state == TS_CREATED);
	kassert(!ke_spinlock_held(&thread->lock));
	kassert(thread->waiting_on == NULL,
	    "retired thread waiting on turnstile");
	kassert(SLIST_EMPTY(&thread->pi_head),
	    "retired thread inheriting priority");
	kassert(thread->turnstile != NULL);

	idalloc_free(&tid_allocator, thread->tid);
}

void
ke_thread_init(kthread_t *thread, ktask_t *task, kturnstile_t *ts,
    void *stack_base, struct karch_trapframe *forkframe, void (*func)(void *),
    void *arg)
{
	ke_spinlock_init(&thread->lock);

	thread->kstack_base = stack_base;
	thread->user = false;
	thread->task = task;

	thread->tid = idalloc_alloc(&tid_allocator);
	thread->tcb = 0;

	thread->state = TS_CREATED;
	thread->sched_class = SCHED_OTHER;
	thread->base_prio = 0;
	thread->inherited_prio = 0;
	thread->effective_prio = 0;
	SLIST_INIT(&thread->pi_head);

	thread->last_cpu_num = KCPUNUM_NULL;
	thread->bound_cpu = KCPUNUM_NULL;
#if 0
	atomic_store_explicit(&thread->runtime, 0, memory_order_relaxed);
#endif

#if defined(__amd64__) /* TODO: extract into ke_md_thread_init_fpu_state? */
	for (size_t i = 0; i < elementsof(thread->pcb.fpu); i++)
		thread->pcb.fpu[i] = 0;
	(*(uint16_t *)(thread->pcb.fpu + 0)) = 0x037f;
	(*(uint32_t *)(thread->pcb.fpu + 24)) = 0x1f80;
#endif


	thread->wait_reason = NULL;

	thread->turnstile = ts;
	thread->waiting_on = NULL;

	atomic_store_explicit(&thread->ast_pending, 0, memory_order_relaxed);

	kep_arch_thread_init(thread, stack_base, forkframe, func, arg);
}

void
ke_task_attach_thread_locked(ktask_t *task, kthread_t *thread)
{
	kassert(ke_spinlock_held(&task->threads_lock));
	kassert(thread->task == task);
	LIST_INSERT_HEAD(&task->threads, thread, proc_link);
	task->threads_count++;
}

void
ke_proc_init(ktask_t *task)
{
	ke_spinlock_init(&task->threads_lock);
	LIST_INIT(&task->threads);
	task->threads_count = 0;
}
