#include <sys/wait.h>

#include <libkern/libkern.h>
#include <posix/proc.h>
#include <posix/sys.h>

#include <sched.h>
#include <string.h>

#include "nanokern/thread.h"

/* use reason 'acquire proclock' to make clearer (easy to confuse proc_lock with
 * proc->lock)*/
kmutex_t   proc_lock;
kprocess_t kproc1;
proc_t	   proc0, proc1;

uint64_t pidrotor = 2;

void
exec_init(void)
{
	const char *argv[] = { "hello", NULL };
	const char *envp[] = { NULL };
	uintptr_t   err;
	int	    r;

	r = syscall3(kPXSysExecVE, (uintptr_t) "/hello", (uintptr_t)argv,
	    (uintptr_t)envp, &err);

	kfatal("failed to exec init: r %d, err %lu\n", r, err);
}

void
posix_init(void)
{
	ipl_t ipl;

	/*
	 * setup posix state for kprocs 0 and 1, dissociate this thread from
	 * kproc0, and associate it with kproc1
	 */

	kproc0.psxproc = &proc0;
	proc0.kproc = &kproc0;
	procx_init(&proc0, NULL);

	proc1.kproc = &kproc1;
	kproc1.pid = 1;
	kproc1.psxproc = &proc1;
	nk_spinlock_init(&kproc1.lock);
	kproc1.map = vm_map_fork(kproc0.map);

	ipl = splhigh();
	SLIST_REMOVE(&kproc0.threads, curthread(), kthread, kproc_link);
	SLIST_INSERT_HEAD(&kproc1.threads, curthread(), kproc_link);
	curthread()->process = &kproc1;
	vm_activate(kproc1.map);
	splx(ipl);

	kmem_dump();

	kprintf("POSIX: loading init\n");
	exec_init();
}

void
procx_init(proc_t *proc, proc_t *super) LOCK_REQUIRES(super->lock)
{
	nk_mutex_init(&proc->lock);
	nk_event_init(&proc->statechange, false);
	LIST_INIT(&proc->subprocs);
	proc->parent = super;
	if (super) {
		LIST_INSERT_HEAD(&super->subprocs, proc, subprocs_link);
	}
#if 0
	proc->pgrp = NULL;
#endif
	memset(proc->files, 0, sizeof(proc->files));
}

/*!
 * reparent subprocesses of \p proc to \p proc's parent.
 *
 * \pre proclock held
 */
void
reparent_processes(proc_t *proc)
{
	proc_t *subproc;
	LIST_FOREACH (subproc, &proc->subprocs, subprocs_link) {
		nk_assert(subproc->parent = proc);
		LIST_REMOVE(subproc, subprocs_link);
		subproc->parent = proc->parent;
		LIST_INSERT_HEAD(&proc->parent->subprocs, subproc,
		    subprocs_link);
	}
}

kx_noreturn int
sys_exit(proc_t *proc, int code)
{
	kwaitstatus_t ws;
	kthread_t    *thread;
	ipl_t	      ipl;

	ws = nk_wait(&proc_lock, "sys_exit: acquire proclock", false, false,
	    -1);

	kassert(ws == kKernWaitStatusOK);

	kassert(proc->state == kPASProcNormal);
	proc->state = kPASProcCompleted;

	SLIST_FOREACH(thread, &proc->kproc->threads, kproc_link)
	{
		ipl = nk_spinlock_acquire(&proc->kproc->lock);
		if (thread == curthread())
			continue;
		else
			nk_fatal("can't exit multiple yet\n");
#if 0
		thread->should_exit = true;
		if (thread->state == kWaiting) {
			waitq_lock(thread->wq);
			waitq_clear_locked(thread, kWaitQResultInterrupted);
			waitq_unlock(thread->wq);
		} else if (thread->state == kRunning && !thread->in_syscall) {
			/* do an IPI or something if it's nonlocal */
			thread->should_exit = true;
			thread->state = kExiting;
		}
#endif
		nk_spinlock_release(&proc->kproc->lock, ipl);
	}

	/* all other threads now exited */

	reparent_processes(proc);

	curthread()->state = kThreadStateDone;
	spldispatch();
	nkx_do_reschedule(kSPLHigh);
	nk_fatal("unreached\n");
}
int
sys_waitpid(proc_t *proc, pid_t pid, int *status, int flags, uintptr_t *errp)
{
	proc_t *subproc;

	kassert((flags & WNOHANG) == 0);
	if (pid != 0 && pid != -1) {
		kfatal("sys_waitpid: unsupported pid %d\n", pid);
	}

	for (;;) {
		/* todo(low): cancellation point */
		kwaitstatus_t ws = nk_wait(&proc_lock,
		    "waitpid: acquire proclock", true, true, -1);
		kassert(ws == kKernWaitStatusOK);
		LIST_FOREACH (subproc, &proc->subprocs, subprocs_link) {
			if (subproc->state == kPASProcCompleted) {
				*status = subproc->wstat;
				/* free the subprocess now? */
				nk_mutex_release(&proc->lock);
				return subproc->kproc->pid;
			}
		}

		nk_mutex_release(&proc->lock);

		ws = nk_wait(&proc->statechange, "waitpid: proc->statechange",
		    true, true, -1);
		kassert(ws == kKernWaitStatusOK);
	}
}
