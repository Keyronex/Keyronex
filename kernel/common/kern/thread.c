/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file thread.c
 * @brief Kernel threads.
 */

#include <keyronex/intr.h>
#include <keyronex/ktask.h>
#include <keyronex/kwait.h>

#include <sched.h>

void
ke_thread_init(kthread_t *thread, ktask_t *task, kturnstile_t *ts,
    void *stack_base, struct karch_trapframe *forkframe, void (*func)(void *),
    void *arg)
{
	ke_spinlock_init(&thread->lock);

	thread->kstack_base = stack_base;
	thread->user = false;

#if 0
	thread->tid = idalloc_alloc(&tid_allocator);
	thread->tcb = 0;
#endif

	thread->state = TS_CREATED;
	thread->sched_class = SCHED_OTHER;
	thread->prio = 0;
	thread->last_cpu_num = KCPUNUM_NULL;
#if 0
	atomic_store_explicit(&thread->runtime, 0, memory_order_relaxed);
#endif

	thread->wait_reason = NULL;

	thread->turnstile = ts;
	thread->waiting_on = NULL;
	thread->sync_ops = NULL;

	kep_arch_thread_init(thread, stack_base, forkframe, func, arg);

#if 0
	ipl = spinlock_lock(&proc->threads_lock);
	TAILQ_INSERT_TAIL(&proc->threads, thread, threads_qlink);
	proc->threads_count++;
	spinlock_unlock(&proc->threads_lock, ipl);
#endif
}
