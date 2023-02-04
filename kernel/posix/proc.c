#include <sys/wait.h>

#include <libkern/libkern.h>
#include <posix/proc.h>

#include <string.h>
#include "nanokern/thread.h"

/* use reason 'acquire proclock' to make clearer (easy to confuse proc_lock with
 * proc->lock)*/
kmutex_t proc_lock;

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
	proc->pgrp = NULL;
	memset(proc->files, 0, sizeof(proc->files));
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

		ws = nk_wait(&proc->statechange, "waitpid: proc->statechange", true, true, -1);
		kassert(ws == kKernWaitStatusOK);
	}
}
