/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Fri Apr 17 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file procdescfs.c
 * @brief Process descriptors filesystem - files representing processes.
 *
 * Process descriptors allow a process to be represented as a file descriptor.
 * When the process exits, EPOLLIN and EPOLLHUP are delivered on the FD.
 * This enables waiting on process exit via epoll/poll alongside other FDs.
 *
 * Locking Notes
 * -------------
 *
 * The order is:
 *
 * 	proctree_mutex
 * 		procdesc::lock
 * 			procdesc::pollhead.lock
 * 				epoll::ready_lock
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/libkern.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/wait.h>

#define PD_CLOEXEC 0x1

/*
 * (p) proctree_lock
 * (l) procdesc::lock
 * (~) invariant after creation
 *
 * proc is a non-owning back pointer. The process holds a reference to the
 * procdesc. It's set to NULL when the process exits or the procdesc is
 * orphaned.
 */
struct procdesc {
	kspinlock_t lock;
	pid_t	pid;		/* (~) pid of process */
	bool	exited;		/* (l) whether process has exited  */
	int	exit_status;	/* (l) if exited, status  */
	proc_t	*proc;		/* (p) process referred to */
	pollhead_t pollhead; /* pollers  */
	kevent_t wait_event;  /* event for blocking pdwait */
};

extern kmutex_t proctree_mutex;

static struct vnode_ops procdesc_vnops;

static inline struct procdesc *
VTOPD(struct vnode *vnode)
{
	kassert(vnode->ops == &procdesc_vnops);
	return (struct procdesc *)vnode->fsprivate_1;
}

static void
procdesc_init(struct procdesc *pd, proc_t *child)
{
	ke_spinlock_init(&pd->lock);
	pd->proc = child;
	pd->pid = child->pid;
	pollhead_init(&pd->pollhead);
	pd->exited = false;
	pd->exit_status = 0;
	ke_event_init(&pd->wait_event, false);

	child->procdesc = pd;
}

static file_t *
procdesc_create(struct procdesc *pd, int flags)
{
	vnode_t *vnode;
	file_t *file;
	int fileflags = 0;

	vnode = vn_alloc(NULL, VCHR, &procdesc_vnops, (uintptr_t)pd, 0);
	if (vnode == NULL)
		return NULL;

	file = file_new((namecache_handle_t) { NULL, NULL }, vnode, fileflags);
	if (file == NULL) {
		vn_release(vnode);
		return NULL;
	}

	return file;
}

/* Notify the process descriptor that its process has exited. */
void
procdesc_notify_exit(struct procdesc *pd, int exit_status)
{
	ipl_t ipl;

	ipl = ke_spinlock_enter(&pd->lock);

	pd->proc = NULL;
	pd->exited = true;
	pd->exit_status = exit_status;

	/* Wake pdwait()'ers. */
	ke_event_set_signalled(&pd->wait_event, true);
	/* Wake epoll()'ers. */
	pollhead_deliver_events(&pd->pollhead, EPOLLIN | EPOLLHUP);

	ke_spinlock_exit(&pd->lock, ipl);
}

/*
 * Orphan a process descriptor (after process reaped by wait4 or similar).
 * Just detaches the proc from the procdesc (the exit status will have been
 * conveyed to the procdesc already)
 *
 * @pre proctree_mutex MUST be held.
 */
void
procdesc_orphan(struct procdesc *pd)
{
	pd->proc = NULL;
}

/*
 * Procdesc vnode ops
 */

static int
procdesc_chpoll(vnode_t *vn, struct poll_entry *pe, enum chpoll_mode mode)
{
	struct procdesc *pd = VTOPD(vn);
	int r;
	ipl_t ipl;

	if (mode == CHPOLL_UNPOLL) {
		kassert(pe != NULL);
		pollhead_unregister(&pd->pollhead, pe);
		return 0;
	}

	if (pe != NULL)
		pollhead_register(&pd->pollhead, pe);

	ipl = ke_spinlock_enter(&pd->lock);

	if (pd->exited)
		r = EPOLLIN | EPOLLHUP;
	else
	 	r = 0;

	ke_spinlock_exit(&pd->lock, ipl);

	return r;
}

static int
procdesc_inactive(vnode_t *vn)
{
	struct procdesc *pd = VTOPD(vn);

	ke_mutex_enter(&proctree_mutex, "procdesc_inactive");
	if (pd->proc != NULL)
		pd->proc->procdesc = NULL;
	ke_mutex_exit(&proctree_mutex);

	kmem_free(pd, sizeof(*pd));
	return 0;
}

static struct vnode_ops procdesc_vnops = {
	.inactive = procdesc_inactive,
	.chpoll = procdesc_chpoll,
};

/*
 * Procdesc syscalls
 */
static void
fork_thread(void *arg)
{
	/* nothing to do yet */
}

pid_t sys_pdfork(karch_trapframe_t *frame, int *fdp, int flags)
{
	proc_t *child_proc;
	thread_t *child_thread;
	struct procdesc *pd;
	file_t *file;
	pid_t pid;
	int fd;
	int r;

	pd = kmem_alloc(sizeof(*pd));
	if (pd == NULL)
		return -ENOMEM;

	file = procdesc_create(pd, flags);
	if (file == NULL) {
		kmem_free(pd, sizeof(*pd));
		return -ENOMEM;
	}

	fd = uf_reserve_fd(curproc()->finfo, 0,
	    (flags & PD_CLOEXEC) ? FD_CLOEXEC : 0);
	if (fd < 0) {
		file_release(file);
		kmem_free(pd, sizeof(*pd));
		return fd;
	}

	child_proc = proc_create(curproc(), true);
	if (child_proc == NULL) {
		file_release(file);
		kmem_free(pd, sizeof(*pd));
		return -ENOMEM;
	}

	pid = child_proc->pid;
	procdesc_init(pd, child_proc);

	child_thread = proc_new_thread(child_proc, frame, fork_thread, NULL);
	if (child_thread == NULL) {
		kfatal("todo: cleanup proc!\n");
		return -ENOMEM;
	}

	child_thread->kthread.user = true;
	ke_thread_copy_fpu_state(&child_thread->kthread);
	child_thread->kthread.tcb = curthread()->kthread.tcb;

	uf_install_reserved(curproc()->finfo, fd, file);

	r = memcpy_to_user(fdp, &fd, sizeof(fd));
	if (r != 0) {
		/* TODO: reserve FD in advance, copyout first, clean up */
		return -EFAULT;
	}

	ke_thread_resume(&child_thread->kthread, false);

	return pid;
}

pid_t
sys_pdwait(int pd_fd, int *status, int options,
    struct rusage *ru, siginfo_t *si)
{
	proc_t *proc = curproc();
	struct file *file;
	struct procdesc *pd;
	vnode_t *vn;
	pid_t pid;
	int exit_status;
	ipl_t ipl;
	int r;

	file = uf_lookup(proc->finfo, pd_fd);
	if (file == NULL)
		return -EBADF;

	vn = file->vnode;
	if (vn->ops != &procdesc_vnops) {
		file_release(file);
		return -EINVAL;
	}

	pd = VTOPD(vn);

	for (;;) {
		ipl = ke_spinlock_enter(&pd->lock);

		if (pd->exited) {
			pid = pd->pid;
			exit_status = pd->exit_status;
			ke_spinlock_exit(&pd->lock, ipl);

			if (status != NULL) {
				r = memcpy_to_user(status, &exit_status,
				    sizeof(exit_status));
				if (r != 0) {
					file_release(file);
					return -EFAULT;
				}
			}

			return pid;
		}

		if (options & WNOHANG) {
			ke_spinlock_exit(&pd->lock, ipl);
			file_release(file);
			return 0;
		}

		ke_spinlock_exit(&pd->lock, ipl);

		r = ke_wait1(&pd->wait_event, "pdwait", false,
		    ABSTIME_FOREVER);
		if (r == -EINTR) {
			file_release(file);
			return -EINTR;
		}
	}

	kunreachable();
}
