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
 */

#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/process.h"
#include "kernel/ke_internal.h"
#include "posix/pxp.h"

posix_proc_t posix_proc0;
static posix_proc_t *posix_proc1;
// static uint64_t pid_counter = 1;

static void
fork_init(void *)
{
	kdprintf("FORK INIT!\n");
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
	proc_init_common(newproc, proc, eproc);

	return 0;
}

int
psx_init(void)
{
	int r;

	proc_init_common(&posix_proc0, NULL, &kernel_process);

	r = psx_fork(NULL, &posix_proc0, &posix_proc1);
	kassert(r == 0);

	kdprintf("Forked off Init\n");

	return 0;
}