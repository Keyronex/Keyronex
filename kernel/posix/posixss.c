/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 22 2023.
 */
/*!
 * @file posix/posixss.c
 * @brief POSIX subsystem routines.
 *
 * Forking
 * -------
 *
 * Forks are arranged to return directly to userland.
 *
 * Lifetimes
 * ---------
 *
 * - The lifetime of an eprocess_t associated with a POSIX process is shorter
 *   than the lifetime of the associated posix_proc_t.
 * - Accordingly if going from an eprocess_t to a posix_proc_t, unless it is via
 *   the current thread (since the current thread will be maintaining the
 *   posix_proc_t), the posix proctree lock should be held.
 *
 */

#include <keyronex/syscall.h>

#include "abi-bits/fcntl.h"
#include "abi-bits/wait.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kernel/ke_internal.h"
#include "posix/pxp.h"

int pxp_make_syscon_tty(void);
int posix_do_openat(vnode_t *dvn, const char *path, int mode);

posix_proc_t posix_proc0;
static posix_proc_t *posix_proc1;
kmutex_t px_proctree_mutex;
static TAILQ_HEAD(, posix_proc) psx_allprocs = TAILQ_HEAD_INITIALIZER(
    psx_allprocs);

static void
fork_init(void *unused)
{
	const char *argv[] = { "bash", "-l", NULL };
	const char *envp[] = { NULL };
	uintptr_t err;
	int r;

	r = posix_do_openat(dev_vnode, "console", O_RDWR);
	kassert(r == 0);

	r = posix_do_openat(dev_vnode, "console", O_RDWR);
	kassert(r == 1);

	r = posix_do_openat(dev_vnode, "console", O_RDWR);
	kassert(r == 2);

	r = syscall3(kPXSysExecVE, (uintptr_t) "/usr/bin/bash", (uintptr_t)argv,
	    (uintptr_t)envp, &err);

	kfatal("failed to exec init: r %d, err %lu\n", r, err);
}

static void
proc_init_common(posix_proc_t *proc, posix_proc_t *parent_proc,
    eprocess_t *eproc)
{
	proc->eprocess = eproc;
	proc->parent = parent_proc;
	LIST_INIT(&proc->subprocs);

	if (parent_proc)
		LIST_INSERT_HEAD(&parent_proc->subprocs, proc, subprocs_link);

	TAILQ_INSERT_TAIL(&psx_allprocs, proc, allprocs_link);

	proc->exited = false;
	proc->wait_stat = 0;
	ke_event_init(&proc->subproc_state_change, false);

	eproc->pas_proc = proc;
}

int
psx_fork(hl_intr_frame_t *frame, posix_proc_t *proc, posix_proc_t **out)
{
	eprocess_t *eproc;
	ethread_t *ethread;
	posix_proc_t *newproc;
	int r;

	r = ps_process_create(&eproc, ps_curproc());
	if (r != 0)
		return r;

	r = ps_thread_create(&ethread, eproc);
	if (r != 0) {
		obj_direct_release(eproc);
		return r;
	}

	if (frame) {
		ethread->kthread.frame = *frame;
		/* todo: move to MD */
		ethread->kthread.frame.rax = 0;
		ethread->kthread.hl.fs = ke_curthread()->hl.fs;
	} else {
		/* we fork without a frame in this case */
		kassert(proc == &posix_proc0);

		kmd_thread_init(&ethread->kthread, fork_init, NULL);
	}

	newproc = kmem_alloc(sizeof(*newproc));
	px_acquire_proctree_mutex();
	proc_init_common(newproc, proc, eproc);
	px_release_proctree_mutex();

	*out = newproc;

	ki_thread_start(&ethread->kthread);

	return 0;
}

int
psx_exit(int status)
{
	posix_proc_t *proc = px_curproc(), *subproc, *tmp;
	vm_map_t *map;

	kassert(proc->eprocess->id != 1);

	/* todo: multithread support */

	px_acquire_proctree_mutex();

	LIST_FOREACH_SAFE (subproc, &proc->subprocs, subprocs_link, tmp) {
		kdprintf("warning: Subproc %u is still around!!\n",
		    subproc->eprocess->id);
		/* re-parent processes logic goes here */
	}

#if 0
	kdprintf("Proc %d (parent %d) - exits\n", proc->eprocess->id,
	    proc->parent->eprocess->id);
#endif

	proc->wait_stat = W_EXITCODE(status, 0);
	proc->exited = true;
	ke_event_signal(&proc->parent->subproc_state_change);
	px_release_proctree_mutex();


	ke_wait(&proc->eprocess->fd_mutex, "psx_exit:eproc->fd_mutex", false, false, -1);
	for (int i = 0; i < elementsof(proc->eprocess->files); i++) {
		if (proc->eprocess->files[i] != NULL)
			obj_direct_release(proc->eprocess->files[i]);
	}

	map = proc->eprocess->map;
	proc->eprocess->map = kernel_process.map;
	vm_map_activate(kernel_process.map);
	vm_map_free(map);

	ke_acquire_dispatcher_lock();
	ke_curthread()->state = kThreadStateDone;
	ki_reschedule();

	kfatal("Unreachable\n");
}

pid_t
psx_waitpid(pid_t pid, int *status, int flags)
{
	posix_proc_t *subproc, *proc = px_curproc();
	kwaitstatus_t w;
	int r = 0;

#if 0
	kdprintf("Proc %u wants to watch on %d, flags %x\n", proc->eprocess->id,
	    pid, flags);
#endif

	kassert((flags & WNOHANG) == 0);
	kassert((flags & WNOWAIT) == 0);

	// kassert(pid == 0 || pid == -1);

dowait:
	w = ke_wait(&proc->subproc_state_change,
	    "psx_waitpid:subproc_state_change", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	px_acquire_proctree_mutex();
	LIST_FOREACH (subproc, &proc->subprocs, subprocs_link) {
		if (subproc->exited) {
			*status = subproc->wait_stat;
			r = subproc->eprocess->id;
			LIST_REMOVE(subproc, subprocs_link);
			TAILQ_REMOVE(&psx_allprocs, subproc, allprocs_link);
			/* free the process stucture */
			break;
		}
	}

	if (r == 0) {
#if 0
		kdprintf("Proc %d: Nothing doing, go back to sleep.\n",
		    proc->eprocess->id);
#endif
		ke_event_clear(&proc->subproc_state_change);
		px_release_proctree_mutex();
		goto dowait;
	}

	px_release_proctree_mutex();

	return r;
}

int
psx_init(void)
{
	int r;

	ke_mutex_init(&px_proctree_mutex);

	proc_init_common(&posix_proc0, NULL, &kernel_process);

	r = vfs_mountdev1();
	kassert(r == 0);

	r = pxp_make_syscon_tty();
	kassert(r == 0);

	kdprintf("Launch POSIX init...\n");
	r = psx_fork(NULL, &posix_proc0, &posix_proc1);
	kassert(r == 0);

	return 0;
}

void dbg_dump_proc_threads(eprocess_t *eproc);

void
dbg_psx_dump_all_procs(void)
{
	posix_proc_t *proc;

	TAILQ_FOREACH (proc, &psx_allprocs, allprocs_link) {
		kdprintf("Proc %d:\n", proc->eprocess->id);
		if (proc->exited) {
			kdprintf(" Exited, status %d.\n", proc->wait_stat);
		} else {
			dbg_dump_proc_threads(proc->eprocess);
		}
	}
}