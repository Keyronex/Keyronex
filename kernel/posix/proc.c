#include <sys/wait.h>

#include <libkern/libkern.h>
#include <posix/proc.h>
#include <posix/sys.h>

#include <string.h>

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
	int r;

	r = syscall3(kPXSysExecVE, (uintptr_t)"/hello", (uintptr_t)argv,
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

		ws = nk_wait(&proc->statechange, "waitpid: proc->statechange",
		    true, true, -1);
		kassert(ws == kKernWaitStatusOK);
	}
}
