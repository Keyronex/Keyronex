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

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/process.h"
#include "kernel/ke_internal.h"
#include "posix/pxp.h"

posix_proc_t posix_proc0;
static posix_proc_t *posix_proc1;
kmutex_t px_proctree_mutex;

static void
fork_init(void *unused)
{
	/* can't do anything here yet */
#if 0
	kdprintf("FORK INIT!\n");
#endif
	for (;;)
		;
}

static void
proc_init_common(posix_proc_t *proc, posix_proc_t *parent_proc,
    eprocess_t *eproc)
{
	proc->eprocess = eproc;
	proc->parent = parent_proc;
	LIST_INIT(&proc->subprocs);

	proc->wait_stat = 0;
	ke_event_init(&proc->subproc_state_change, false);
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
		ethread->kthread.frame.rdi = 0;
	} else {
		/* we fork without a frame in this case */
		kassert(proc == &posix_proc0);

		kmd_thread_init(&ethread->kthread, fork_init, NULL);
	}

	newproc = kmem_alloc(sizeof(*newproc));
	px_acquire_proctree_mutex();
	proc_init_common(newproc, proc, eproc);
	px_release_proctree_mutex();

	ki_thread_start(&ethread->kthread);

	return 0;
}

int
psx_init(void)
{
	int r;

	ke_mutex_init(&px_proctree_mutex);

	proc_init_common(&posix_proc0, NULL, &kernel_process);

	r = psx_fork(NULL, &posix_proc0, &posix_proc1);
	kassert(r == 0);

	kdprintf("Forked off Init\n");

	return 0;
}