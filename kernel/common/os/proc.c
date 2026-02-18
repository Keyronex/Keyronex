/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file proc.c
 * @brief Process & thread objects.
 */

#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/pcb.h>
#include <sys/proc.h>

#include <stdalign.h>

kmem_cache_t *turnstile_cache, *proc_cache, *thread_cache;

void
proc_init(void)
{
	proc_cache = kmem_cache_create("proc", sizeof(proc_t), _Alignof(proc_t),
	    NULL);
	thread_cache = kmem_cache_create("thread",
	    roundup2(sizeof(thread_t), 16), MIN2(alignof(thread_t), 16), NULL);
	turnstile_cache = kmem_cache_create("Turnstile", sizeof(kturnstile_t),
	    alignof(kturnstile_t), NULL);
}

thread_t *
proc_alloc_idle_thread(void)
{
	thread_t *thread;
	kturnstile_t *ts;

	thread = kmem_cache_alloc(thread_cache, 0);
	if (thread == NULL)
		kfatal("couldn't allocate idle thread");

	ts = kmem_cache_alloc(turnstile_cache, 0);
	if (ts == NULL)
		kfatal("couldn't allocate idle turnstile");

	thread->kthread.turnstile = ts;

	return thread;
}

thread_t *
proc_new_thread(proc_t *proc, karch_trapframe_t *fork_frame,
    void (*func)(void *), void *arg)
{
	thread_t *thread;
	void *stack;
	kturnstile_t *ts;

	thread = kmem_cache_alloc(thread_cache, 0);
	if (thread == NULL)
		return NULL;

	ts = kmem_cache_alloc(turnstile_cache, 0);
	if (ts == NULL) {
		kmem_cache_free(thread_cache, thread);
		return NULL;
	}

	stack = vm_kwired_alloc(KSTACK_SIZE / PGSIZE, 0);
	if (stack == NULL) {
		kmem_cache_free(thread_cache, thread);
		kmem_cache_free(turnstile_cache, ts);
		return NULL;
	}

	ke_thread_init(&thread->kthread, &proc->ktask, ts, stack,
		fork_frame, func, arg);

	return thread;
}

thread_t *
proc_new_system_thread(void (*func)(void *), void *arg)
{
	return proc_new_thread(&proc0, NULL, func, arg);
}
