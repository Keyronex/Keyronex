/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file thread.c
 * @brief Kernel threads.
 */

#include <sys/k_intr.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>

#include <libkern/idalloc.h>

#include <sched.h>

uint8_t tid_bitmap[UINT16_MAX / 8];
struct id_allocator tid_allocator = IDALLOC_INITIALISER(tid_bitmap, UINT16_MAX);

void
ke_thread_init(kthread_t *thread, ktask_t *task, kturnstile_t *ts,
    void *stack_base, struct karch_trapframe *forkframe, void (*func)(void *),
    void *arg)
{
	ipl_t ipl;

	ke_spinlock_init(&thread->lock);

	thread->kstack_base = stack_base;
	thread->user = false;
	thread->task = task;

	thread->tid = idalloc_alloc(&tid_allocator);
	thread->tcb = 0;

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

	kep_arch_thread_init(thread, stack_base, forkframe, func, arg);

	ipl = ke_spinlock_enter(&task->threads_lock);
	LIST_INSERT_HEAD(&task->threads, thread, proc_link);
	task->threads_count++;
	ke_spinlock_exit(&task->threads_lock, ipl);
}

void
ke_set_tcb(uintptr_t value)
{
	ke_curthread()->tcb = value;
}


void ke_proc_init(ktask_t *task)
{
	ke_spinlock_init(&task->threads_lock);
	LIST_INIT(&task->threads);
	task->threads_count = 0;
}
