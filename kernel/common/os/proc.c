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
#include <sys/krx_user.h>
#include <sys/libkern.h>
#include <sys/pcb.h>
#include <sys/proc.h>

#include <stdalign.h>

kmem_cache_t *turnstile_cache, *proc_cache, *thread_cache;
_Atomic(pid_t) last_pid = 1;
struct session session0;
struct pgrp pgrp0;
TAILQ_HEAD(, proc) allproc;
kmutex_t proctree_mutex = KMUTEX_INITIALISER;

void
proc_init(void)
{
	proc_cache = kmem_cache_create("proc", sizeof(proc_t), _Alignof(proc_t),
	    NULL);
	thread_cache = kmem_cache_create("thread",
	    roundup2(sizeof(thread_t), 16), MIN2(alignof(thread_t), 16), NULL);
	turnstile_cache = kmem_cache_create("Turnstile", sizeof(kturnstile_t),
	    alignof(kturnstile_t), NULL);

	proc0.pid = 0;

	pgrp0.refcount = 1;
	pgrp0.pgid = 0;
	pgrp0.session = &session0;
	LIST_INIT(&pgrp0.members);
	LIST_INSERT_HEAD(&session0.pgrps, &pgrp0, session_link);

	session0.refcount = 1;
	session0.sid = 0;
	session0.leader = &proc0;
	session0.ctty_vn = NULL;
	session0.ctty_str = NULL;
	LIST_INIT(&session0.pgrps);

	proc0.pgrp = &pgrp0;
	LIST_INSERT_HEAD(&pgrp0.members, &proc0, pgrp_link);

	strcpy(proc0.comm, "(swapper)");
	TAILQ_INIT(&proc0.children);

	TAILQ_INIT(&allproc);
	TAILQ_INSERT_TAIL(&allproc, &proc0, allproc_qlink);
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

	thread->vm_map = NULL;
	thread->kthread.turnstile = ts;

	return thread;
}


proc_t *
proc_create(proc_t *parent, bool fork)
{
	proc_t *proc;

	proc = kmem_cache_alloc(proc_cache, VM_SLEEP);
	if (proc == NULL)
		return NULL;

	strcpy(proc->comm, "unnamed");

	proc->vm_map = vm_map_create();
	proc->finfo = uf_new();
	proc->pid = atomic_fetch_add(&last_pid, 1);

	if (fork)
		vm_fork(parent->vm_map, proc->vm_map);

	ke_proc_init(&proc->ktask);

	proc->parent = parent;
	TAILQ_INIT(&proc->children);
	proc->exited = false;

	proc->wait_ev = NULL;
	proc->procdesc = NULL;

	ke_mutex_enter(&proctree_mutex, "proc_create");
	proc->pgrp = NULL;
	pgrp_add_member(parent->pgrp, proc);

	TAILQ_INSERT_TAIL(&allproc, proc, allproc_qlink);
	TAILQ_INSERT_TAIL(&parent->children, proc, sibling_qlink);
	ke_mutex_exit(&proctree_mutex);
	return proc;
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

	thread->vm_map = NULL;

	ke_thread_init(&thread->kthread, &proc->ktask, ts, stack,
		fork_frame, func, arg);

	return thread;
}

thread_t *
proc_new_system_thread(void (*func)(void *), void *arg)
{
	return proc_new_thread(&proc0, NULL, func, arg);
}

void
thread_activate(thread_t *old, thread_t *new)
{
	void pmap_activate(vm_map_t *map);
	if (thread_vm_map(old) != thread_vm_map(new))
		pmap_activate(thread_vm_map(new));
}

pid_t
sys_getppid(proc_t *proc)
{
	pid_t ppid;
	ke_mutex_enter(&proctree_mutex, "sys_getppid");
	ppid = proc->parent ? proc->parent->pid : 0;
	ke_mutex_exit(&proctree_mutex);
	return ppid;
}
