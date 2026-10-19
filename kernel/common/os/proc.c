/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file proc.c
 * @brief Process & thread objects.
 */

#include <sys/errno.h>
#include <sys/k_cpumask.h>
#include <sys/k_log.h>
#include <sys/k_wait.h>
#include <sys/k_xcall.h>
#include <sys/kmem.h>
#include <sys/krx_user.h>
#include <sys/libkern.h>
#include <sys/pcb.h>
#include <sys/proc.h>
#include <sys/wait.h>

#include <libkern/idalloc.h>
#include <stdalign.h>

void vm_map_activate(vm_map_t *old, vm_map_t *new);

#define PID_MAX 65535

kmem_cache_t *turnstile_cache, *proc_cache, *thread_cache;
static uint8_t pid_bitmap[(PID_MAX + 1) / 8];
static struct id_allocator pid_allocator = IDALLOC_INITIALISER(pid_bitmap,
    PID_MAX);
proc_t *initproc;
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

	proc0.pid = idalloc_alloc(&pid_allocator);
	kassert(proc0.pid == 0);
	proc0.lifecycle = PROC_RUNNING;
	ke_event_init(&proc0.child_ev, false);
	ke_event_init(&proc0.thread_ev, false);

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

proc_t *
proc_curproc(void)
{
	return curproc();
}

/* @pre proctree_mutex held */
proc_t *
proc_find_locked(pid_t pid)
{
	proc_t *proc;

	kassert(ke_mutex_held(&proctree_mutex));

	TAILQ_FOREACH(proc, &allproc, allproc_qlink) {
		if (proc->pid == pid)
			return proc;
	}

	return NULL;
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

	if (!fork)
		strcpy(proc->comm, "unnamed");
	else
	 	strcpy(proc->comm, parent->comm);

	proc->pid = idalloc_alloc(&pid_allocator);
	if (proc->pid < 0) {
		kmem_cache_free(proc_cache, proc);
		return NULL;
	}

	proc->vm_map = vm_map_create();

	if (fork) {
		proc->finfo = uf_fork(parent->finfo);
		vm_fork(parent->vm_map, proc->vm_map);
	} else {
		proc->finfo = uf_new();
	}

	ke_proc_init(&proc->ktask);

	proc->parent = parent;
	TAILQ_INIT(&proc->children);
	proc->exited = false;
	proc->lifecycle = PROC_RUNNING;
	proc->exit_status = 0;
	ke_event_init(&proc->child_ev, false);
	ke_event_init(&proc->thread_ev, false);

	proc->procdesc = NULL;

	ke_mutex_enter(&proctree_mutex, "proc_create");
	proc->pgrp = NULL;
	pgrp_add_member(parent->pgrp, proc);

	TAILQ_INSERT_TAIL(&allproc, proc, allproc_qlink);
	TAILQ_INSERT_TAIL(&parent->children, proc, sibling_qlink);
	ke_mutex_exit(&proctree_mutex);
	return proc;
}

static void thread_destroy(thread_t *thread);

thread_t *
proc_new_thread(proc_t *proc, karch_trapframe_t *fork_frame,
    void (*func)(void *), void *arg)
{
	thread_t *thread;
	void *stack;
	kturnstile_t *ts;
	ipl_t ipl;

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

	ipl = ke_spinlock_enter(&proc->ktask.threads_lock);
	if (proc->lifecycle != PROC_RUNNING) {
		/* process no longer accepting threads, give up */
		ke_spinlock_exit(&proc->ktask.threads_lock, ipl);
		thread_destroy(thread);
		return NULL;
	}
	ke_task_attach_thread_locked(&proc->ktask, &thread->kthread);
	ke_spinlock_exit(&proc->ktask.threads_lock, ipl);

	return thread;
}

thread_t *
proc_new_system_thread(void (*func)(void *), void *arg)
{
	return proc_new_thread(&proc0, NULL, func, arg);
}

static void
thread_destroy(thread_t *thread)
{
	ke_thread_deinit(&thread->kthread);
	kmem_cache_free(turnstile_cache, thread->kthread.turnstile);
	vm_kwired_free(thread->kthread.kstack_base, KSTACK_SIZE / PGSIZE);
	kmem_cache_free(thread_cache, thread);
}

static void
thread_reaper(void *)
{
	kthread_t *kthread;

	for (;;) {
		ke_wait1(&ke_thread_retire_event, "thread_reaper", false,
		    ABSTIME_FOREVER);
		ke_event_set_signalled(&ke_thread_retire_event, false);

		while ((kthread = ke_thread_retire_dequeue()) != NULL) {
			kassert(kthread->task == &proc0.ktask,
			    "retired threads must belong to proc0");
			thread_destroy((thread_t *)kthread);
		}
	}
}

void
proc_reaper_init(void)
{
	thread_t *thread = proc_new_system_thread(thread_reaper, NULL);

	if (thread == NULL)
		kfatal("couldn't create thread reaper");
	ke_thread_resume(&thread->kthread, false);
}

void
thread_activate(thread_t *old, thread_t *new)
{
	vm_map_activate(thread_vm_map(old), thread_vm_map(new));
}

struct thread_new_info {
	uintptr_t entry;
	uintptr_t stack;
};


static void
user_thread_trampoline(void *arg)
{
	struct thread_new_info info = *(struct thread_new_info *)arg;
	void ke_md_enter_usermode(uintptr_t ip, uintptr_t sp);
	kmem_free(arg, sizeof(info));
	ke_md_enter_usermode(info.entry, info.stack);
}

int sys_fork_thread(uintptr_t entry, uintptr_t stack)
{
	thread_t *thread;
	struct thread_new_info *info = kmem_alloc(sizeof(*info));
	uint32_t tid;

	info->entry = entry;
	info->stack = stack;

	thread = proc_new_thread(curproc(), NULL, user_thread_trampoline, info);
	if (thread == NULL) {
		kmem_free(info, sizeof(*info));
		return -EAGAIN;
	}

	tid = thread->kthread.tid;
	ke_thread_resume(&thread->kthread, false);

	return tid;
}

/*
 * thread termination
 */

/*
 * Deliver a termination AST to every thread of this process except the caller.
 *
 * @pre proc->ktask.threads_lock held
 */
static void
terminate_threads_locked(proc_t *proc, kcpumask_t *pokes)
{
	kthread_t *self = ke_curthread(), *thread;

	kassert(ke_spinlock_held(&proc->ktask.threads_lock));

	LIST_FOREACH(thread, &proc->ktask.threads, proc_link) {
		kcpunum_t cpu;

		if (thread == self)
			continue;

		atomic_fetch_or_explicit(&thread->ast_pending, AST_TERMINATE,
		    memory_order_release);
		cpu = ke_thread_interrupt(thread);
		if (cpu != KCPUNUM_NULL)
			ke_cpumask_set(pokes, cpu);
	}
}

void
ke_ast_check(void)
{
	kthread_t *thread = ke_curthread();

	kassert(ke_ipl() == IPL_0);

	if (atomic_load_explicit(&thread->ast_pending, memory_order_acquire) &
	    AST_TERMINATE)
		sys_thread_exit();
}

/* last thread exit, zombify the process */
static void
proc_exit_last(proc_t *proc)
{
	proc_t *child, *tmp;
	struct session *sess;

	if (proc == initproc)
		kfatal("init exited (status %#x)\n", proc->exit_status);

	vm_unmap(proc->vm_map, LOWER_HALF, LOWER_HALF + LOWER_HALF_SIZE);
	vm_map_release(proc->vm_map);
	proc->vm_map = NULL;

	uf_destroy(proc->finfo);
	proc->finfo = NULL;

	ke_mutex_enter(&proctree_mutex, "proc_exit_last");

	TAILQ_FOREACH_SAFE(child, &proc->children, sibling_qlink, tmp) {
		TAILQ_REMOVE(&proc->children, child, sibling_qlink);
		child->parent = initproc;
		TAILQ_INSERT_TAIL(&initproc->children, child, sibling_qlink);
		if (child->exited)
			ke_event_set_signalled(&initproc->child_ev, true);
	}

	sess = proc->pgrp->session;
	if (sess->leader == proc) {
		sess->leader = NULL;
		if (sess->ctty_str != NULL)
			str_tty_detach_session_locked(sess->ctty_str);
	}

	pgrp_remove_member(proc);

	proc->exited = true;
	ke_event_set_signalled(&proc->parent->child_ev, true);
	if (proc->procdesc != NULL)
		procdesc_notify_exit(proc->procdesc, proc->exit_status);

	ke_mutex_exit(&proctree_mutex);
}

void
sys_thread_exit(void)
{
	thread_t *curthread = curthread();
	proc_t *proc = curproc();
	bool last;
	ipl_t ipl;

	curthread->vm_map = proc0.vm_map;
	vm_map_activate(proc->vm_map, proc0.vm_map);

	ipl = ke_spinlock_enter(&proc->ktask.threads_lock);
	LIST_REMOVE(&curthread->kthread, proc_link);
	last = --proc->ktask.threads_count == 0;
	if (last) {
		/* we are the last to leave */
		if (proc->lifecycle != PROC_EXITING) {
			proc->lifecycle = PROC_EXITING;
			proc->exit_status = W_EXITCODE(0, 0);
		}
	} else if (proc->lifecycle != PROC_RUNNING) {
		/*
		 * possibly being ended via execve() - wake it in case */
		ke_event_set_signalled(&proc->thread_ev, true);
	}
	ke_spinlock_exit(&proc->ktask.threads_lock, ipl);

	/* reassociate to proc0 before reap, but stay off its threads list */
	curthread->kthread.task = &proc0.ktask;

	if (last)
		proc_exit_last(proc);

	ke_thread_exit();
}

/* Terminate a process with given wait status, if not already terminating. */
void
proc_terminate(proc_t *proc, int status)
{
	kthread_t *self = ke_curthread();
	kcpumask_t pokes;
	ipl_t ipl;

	ke_cpumask_zero(&pokes);

	ipl = ke_spinlock_enter(&proc->ktask.threads_lock);
	if (proc->lifecycle != PROC_EXITING) {
		proc->lifecycle = PROC_EXITING;
		proc->exit_status = status;
		terminate_threads_locked(proc, &pokes);
		if (kthread_proc(self) == proc)
			atomic_fetch_or_explicit(&self->ast_pending,
			    AST_TERMINATE, memory_order_release);
	}
	ke_spinlock_exit(&proc->ktask.threads_lock, ipl);

	ke_xcall_nop_multicast(&pokes);
}

void
sys_exit(int status)
{
	proc_terminate(curproc(), W_EXITCODE(status & 0xff, 0));
	sys_thread_exit();
}

int
proc_exec_begin(proc_t *proc)
{
	kcpumask_t pokes;
	ipl_t ipl;
	int r;

	ke_cpumask_zero(&pokes);

	ipl = ke_spinlock_enter(&proc->ktask.threads_lock);

	if (proc->lifecycle != PROC_RUNNING) {
		ke_spinlock_exit(&proc->ktask.threads_lock, ipl);
		return -EINTR;
	}

	if (proc->ktask.threads_count == 1) {
		ke_spinlock_exit(&proc->ktask.threads_lock, ipl);
		return 0;
	}

	/* ban further new threads, make away with what others still exist */
	proc->lifecycle = PROC_EXECING;
	terminate_threads_locked(proc, &pokes);
	ke_spinlock_exit(&proc->ktask.threads_lock, ipl);

	ke_xcall_nop_multicast(&pokes);

	for (;;) {
		ipl = ke_spinlock_enter(&proc->ktask.threads_lock);
		if (proc->ktask.threads_count == 1)
			break;
		ke_event_set_signalled(&proc->thread_ev, false);
		ke_spinlock_exit(&proc->ktask.threads_lock, ipl);
		ke_wait1(&proc->thread_ev, "execve: threads draining", false,
		    ABSTIME_FOREVER);
	}

	/* if another thread called exit() meanwhile, make away ourselves too */
	if (proc->lifecycle == PROC_EXECING) {
		proc->lifecycle = PROC_RUNNING;
		r = 0;
	} else {
		r = -EINTR;
	}
	ke_spinlock_exit(&proc->ktask.threads_lock, ipl);

	return r;
}

static void
proc_free(proc_t *proc)
{
	kassert(proc->vm_map == NULL && proc->finfo == NULL);
	kassert(proc->pgrp == NULL && proc->procdesc == NULL);
	kassert(TAILQ_EMPTY(&proc->children));
	kassert(proc->ktask.threads_count == 0);

	idalloc_free(&pid_allocator, proc->pid);
	kmem_cache_free(proc_cache, proc);
}

/*
 * Reap a zombie process - must have no threads left associated with it.
 *
 * @pre proctree_mutex MUST be held.
 * @pre child->parent == parent && child->exited
 */
void
proc_reap_locked(proc_t *parent, proc_t *child)
{
	kassert(ke_mutex_held(&proctree_mutex));
	kassert(child->parent == parent);
	kassert(child->exited);

	TAILQ_REMOVE(&parent->children, child, sibling_qlink);
	TAILQ_REMOVE(&allproc, child, allproc_qlink);

	pgrp_remove_member(child);

	if (child->procdesc != NULL) {
		procdesc_orphan(child->procdesc);
		child->procdesc = NULL;
	}

	proc_free(child);
}

/* Destroy a process that never ran (i.e. fork() failed) */
void
proc_destroy_unstarted(proc_t *proc)
{
	kassert(proc->ktask.threads_count == 0);

	vm_unmap(proc->vm_map, LOWER_HALF, LOWER_HALF + LOWER_HALF_SIZE);
	vm_map_release(proc->vm_map);
	proc->vm_map = NULL;

	uf_destroy(proc->finfo);
	proc->finfo = NULL;

	ke_mutex_enter(&proctree_mutex, "proc_destroy_unstarted");
	TAILQ_REMOVE(&proc->parent->children, proc, sibling_qlink);
	TAILQ_REMOVE(&allproc, proc, allproc_qlink);
	pgrp_remove_member(proc);
	kassert(proc->procdesc == NULL);
	ke_mutex_exit(&proctree_mutex);

	proc_free(proc);
}

pid_t
sys_wait4(pid_t pid, int *out_ustatus, int flags,
    struct rusage *out_urusage)
{
	proc_t *proc = curproc();

	/* TODO pgrp waits */
	if (!(pid == -1 || pid > 0))
		return -EINVAL;

	ke_mutex_enter(&proctree_mutex, "sys_wait4: proctree_mutex");
	while (true) {
		proc_t *child;
		proc_t *found = NULL;
		bool any = false;
		int r;

		TAILQ_FOREACH(child, &proc->children, sibling_qlink) {
			if (pid > 0 && child->pid != pid)
				continue;
			any = true;
			if (child->exited) {
				found = child;
				break;
			}
		}

		if (found) {
			int status = found->exit_status;
			pid_t ret_pid = found->pid;

			if (!(flags & WNOWAIT))
				proc_reap_locked(proc, found);
			ke_mutex_exit(&proctree_mutex);

			if (out_ustatus != NULL &&
			    memcpy_to_user(out_ustatus, &status,
			    sizeof(status)) != 0)
				return -EFAULT;
			return ret_pid;
		}

		if (!any) {
			ke_mutex_exit(&proctree_mutex);
			return -ECHILD;
		}

		if (flags & WNOHANG) {
			ke_mutex_exit(&proctree_mutex);
			return 0;
		}

		ke_event_set_signalled(&proc->child_ev, false);
		ke_mutex_exit(&proctree_mutex);

		r = ke_wait1(&proc->child_ev, "sys_wait4", true,
		    ABSTIME_FOREVER);
		if (r == -EINTR)
			return -EINTR;

		ke_mutex_enter(&proctree_mutex, "sys_wait4");
	}
}

static void
fork_thread(void *arg)
{
	/* nothing to do yet; just return */
}

pid_t
sys_fork(karch_trapframe_t *frame)
{
	proc_t *proc = curproc();
	proc_t *child_proc;
	thread_t *child_thread;
	pid_t pid;

	child_proc = proc_create(proc, true);
	if (child_proc == NULL)
		return -ENOMEM;

	pid = child_proc->pid;

	child_thread = proc_new_thread(child_proc, frame, fork_thread, NULL);
	if (child_thread == NULL) {
		proc_destroy_unstarted(child_proc);
		return -ENOMEM;
	}

	child_thread->kthread.user = true;
	ke_thread_copy_fpu_state(&child_thread->kthread);
	child_thread->kthread.tcb = curthread()->kthread.tcb;

	ke_thread_resume(&child_thread->kthread, false);

	kassert(pid != 0);

	return pid;
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

void
dbg_proc_dump_all_threads(void)
{
	proc_t *proc;

	/* note: GDB wants executable stack so will fault after this */

	ke_mutex_enter(&proctree_mutex, "dbg_proc_dump_all_threads");
	TAILQ_FOREACH(proc, &allproc, allproc_qlink) {
		kthread_t *thread;
		ipl_t ipl;

		ipl = ke_spinlock_enter(&proc->ktask.threads_lock);
		kdprintf("- process %d (comm %s):\n", proc->pid, proc->comm);
		LIST_FOREACH(thread, &proc->ktask.threads, proc_link) {
			ke_spinlock_enter_nospl(&thread->lock);
			kdprintf("  -> thread %d", thread->tid);
			if (thread->wait_reason != NULL)
				kdprintf(" (waiting on %s)", thread->wait_reason);
			kdprintf("\n");
			ke_spinlock_exit_nospl(&thread->lock);
		}
		splx(ipl);
	}

	ke_mutex_exit(&proctree_mutex);
}
