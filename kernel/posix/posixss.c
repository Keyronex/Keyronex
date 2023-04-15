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

#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <abi-bits/fcntl.h>
#include <keyronex/syscall.h>

#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kernel/ke_internal.h"
#include "posix/pxp.h"
#include "posix/tty.h"

#define DECLARE_SIMPLE_COMPARATOR(TYPE, MEMBER) \
	intptr_t TYPE##_cmp(struct TYPE *x, struct TYPE *y)

#define DEFINE_SIMPLE_COMPARATOR(TYPE, MEMBER)  \
	DECLARE_SIMPLE_COMPARATOR(TYPE, MEMBER) \
	{                                       \
		return x->MEMBER - y->MEMBER;   \
	}

DECLARE_SIMPLE_COMPARATOR(posix_pgroup, pgid);
int pxp_make_syscon_tty(void);
int posix_do_openat(vnode_t *dvn, const char *path, int mode);

posix_proc_t posix_proc0;
static posix_thread_t posix_thread0;
static struct posix_pgroup posix_pgroup0;
static struct posix_session posix_session0;
static posix_proc_t *posix_proc1;
kspinlock_t px_proctree_mutex;
static TAILQ_HEAD(, posix_proc) psx_allprocs = TAILQ_HEAD_INITIALIZER(
    psx_allprocs);
static RB_HEAD(posix_pgroup_rb, posix_pgroup) pgroup_rb;

RB_GENERATE(posix_pgroup_rb, posix_pgroup, rb_entry, posix_pgroup_cmp);
DEFINE_SIMPLE_COMPARATOR(posix_pgroup, pgid);

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

	r = psx_setsid();
	kassert(r == 1);

	/* todo move */
	extern struct tty sctty;
	sctty.pg = px_curproc()->pgroup;

	r = syscall3(kPXSysExecVE, (uintptr_t) "/usr/bin/bash", (uintptr_t)argv,
	    (uintptr_t)envp, &err);

	kfatal("failed to exec init: r %d, err %lu\n", r, err);
}

/*! @pre proctree_lock held */
static void
proc_init_common(posix_proc_t *proc, posix_proc_t *parent_proc,
    posix_thread_t *thread, posix_thread_t *parent_thread, eprocess_t *eproc,
    ethread_t *ethread)
{
	proc->eprocess = eproc;
	proc->parent = parent_proc;
	LIST_INIT(&proc->subprocs);

	if (parent_proc) {
		LIST_INSERT_HEAD(&parent_proc->subprocs, proc, subprocs_link);
		LIST_INSERT_HEAD(&parent_proc->pgroup->members, proc,
		    pgroup_members_link);
		proc->pgroup = parent_proc->pgroup;
	} else {
		kassert(proc == &posix_proc0);
	}

	TAILQ_INSERT_TAIL(&psx_allprocs, proc, allprocs_link);

	proc->exited = false;
	proc->wait_stat = 0;
	ke_event_init(&proc->subproc_state_change, false);

	if (parent_thread) {
		memcpy(thread->sigflags, parent_thread->sigflags,
		    sizeof(thread->sigflags));
		memcpy(thread->sighandlers, parent_thread->sighandlers,
		    sizeof(thread->sighandlers));
		thread->sigmask = parent_thread->sigmask;
		memcpy(thread->sigsigmask, parent_thread->sigsigmask,
		    sizeof(thread->sigsigmask));
	} else {
		thread->sigmask = 0;

		for (size_t i = 0; i < SIGRTMAX; i++) {
			thread->sigflags[i] = 0;
			thread->sighandlers[i].handler = SIG_DFL;
			thread->sigsigmask[i] = 1 << i;
		}
	}

	TAILQ_INIT(&thread->sigqueue);

	eproc->pas_proc = proc;
	ethread->pas_thread = thread;
}

/*! @pre proctree_lock locked */
struct posix_pgroup *
psx_lookup_pgid(pid_t pgid)
{
	struct posix_pgroup key;
	key.pgid = pgid;
	return RB_FIND(posix_pgroup_rb, &pgroup_rb, &key);
}

int
psx_fork(hl_intr_frame_t *frame, posix_proc_t *proc, posix_proc_t **out)
{
	eprocess_t *eproc;
	ethread_t *ethread;
	posix_proc_t *newproc;
	posix_thread_t *newthread;
	ipl_t ipl;
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

	newthread = kmem_alloc(sizeof(posix_thread_t));
	newproc = kmem_alloc(sizeof(*newproc));
	ipl = px_acquire_proctree_mutex();
	proc_init_common(newproc, proc, newthread, px_curthread(), eproc,
	    ethread);
	px_release_proctree_mutex(ipl);

	*out = newproc;

	ki_thread_start(&ethread->kthread);

	return 0;
}

int
psx_exit(int status)
{
	posix_proc_t *proc = px_curproc(), *subproc, *tmp;
	vm_map_t *map;
	ipl_t ipl;

	kassert(proc->eprocess->id != 1);

	/* todo: multithread support */

	ipl = px_acquire_proctree_mutex();

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
	px_release_proctree_mutex(ipl);

	ke_wait(&proc->eprocess->fd_mutex, "psx_exit:eproc->fd_mutex", false,
	    false, -1);
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
psx_getpgid(pid_t pid)
{
	kassert(pid == 0);

	return px_curproc()->pgroup->pgid;
}

pid_t
psx_setpgid(pid_t pid, pid_t pgid)
{
	ipl_t ipl;
	posix_proc_t *proc = NULL;
	struct posix_pgroup *pg, *oldpg;
	pid_t r;

	if (pid < 0 || pgid < 0)
		return -EINVAL;

	ipl = px_acquire_proctree_mutex();

	if (pid == 0) {
		proc = px_curproc();
		pid = proc->eprocess->id;
	} else if (pid == px_curproc()->eprocess->id) {
		proc = px_curproc();
	} else {
		posix_proc_t *child;

		LIST_FOREACH (child, &px_curproc()->subprocs, subprocs_link) {
			if (child->eprocess->id == pid) {
				proc = child;
				break;
			}
		}
		if (!proc) {
			r = -ESRCH;
			goto out;
		} else if (proc->pgroup->session !=
		    px_curproc()->pgroup->session) {
			r = -EPERM;
			goto out;
		}

		/* todo: EACCES if child exec()'d */
	}

	if (pgid == 0 || pgid == pid) {
		if (proc->pgroup->pgid == proc->eprocess->id) {
			/* already in its own group */
			r = pid;
			goto out;
		}

		pg = kmem_alloc(sizeof(*pg));
		pg->pgid = pid;
		pg->session = proc->pgroup->session;
		pg->session->nmembers++;
		LIST_INIT(&pg->members);
		pgid = pid;

		RB_INSERT(posix_pgroup_rb, &pgroup_rb, pg);
	} else {
		pg = psx_lookup_pgid(pgid);
		if (!pg) {
			r = -ESRCH;
			goto out;
		}
	}

	oldpg = proc->pgroup;
	(void)oldpg;
	proc->pgroup = pg;

	kassert(proc);
	kassert(pg);

	kdprintf("Doing it...\n");

	LIST_REMOVE(proc, pgroup_members_link);

	LIST_INSERT_HEAD(&pg->members, proc, pgroup_members_link);

	r = pg->pgid;

out:
	px_release_proctree_mutex(ipl);

	return r;
}

pid_t
psx_getsid(void)
{
	return px_curproc()->pgroup->session->sid;
}

pid_t
psx_setsid(void)
{
	ipl_t ipl;
	posix_proc_t *proc = px_curproc();
	pid_t r;

	ipl = px_acquire_proctree_mutex();

	if (proc->pgroup->session->leader != proc) {
		struct posix_session *sess, *oldsess;
		struct posix_pgroup *pg, *oldpg;

		oldpg = proc->pgroup;
		oldsess = oldpg->session;

		(void)oldsess;
		(void)oldpg;

		sess = kmem_alloc(sizeof(*sess));
		sess->leader = proc;
		sess->nmembers = 1;
		sess->sid = proc->eprocess->id;

		pg = kmem_alloc(sizeof(*pg));
		pg->pgid = sess->sid;
		pg->session = sess;

		proc->pgroup = pg;

		LIST_REMOVE(proc, pgroup_members_link);
		LIST_INSERT_HEAD(&pg->members, proc, pgroup_members_link);

		RB_INSERT(posix_pgroup_rb, &pgroup_rb, pg);

		r = sess->sid;
	} else
		r = -EPERM;

	px_release_proctree_mutex(ipl);

	return r;
}

pid_t
psx_waitpid(pid_t pid, int *status, int flags)
{
	posix_proc_t *subproc, *proc = px_curproc();
	kwaitstatus_t w;
	ipl_t ipl;
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

	ipl = px_acquire_proctree_mutex();
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
		px_release_proctree_mutex(ipl);
		goto dowait;
	}

	px_release_proctree_mutex(ipl);

	return r;
}

static int
psx_domask(int how, const sigset_t *__restrict set,
    sigset_t *__restrict retrieve)
{
	int r = 0;

	if (retrieve != NULL)
		*retrieve = px_curthread()->sigmask;

	if (set == NULL)
		return 0;

	switch (how) {
	case SIG_BLOCK:
		px_curthread()->sigmask &= *set;
		break;

	case SIG_UNBLOCK:
		px_curthread()->sigmask &= ~*set;
		break;

	case SIG_SETMASK:
		px_curthread()->sigmask = *set;
		break;

	default:
		r = -EINVAL;
	}

	return r;
}

int
psx_sigaction(int signal, const struct sigaction *__restrict action,
    struct sigaction *__restrict oldAction)
{
	ipl_t ipl;
	struct sigaction old, new;
	union posix_sighandler *phandler;
	sigset_t *pmask;
	int *pflags;

	if (action != NULL)
		new = *action;

	ipl = px_acquire_proctree_mutex();

	phandler = &px_curthread()->sighandlers[signal];
	pflags = &px_curthread()->sigflags[signal];
	pmask = &px_curthread()->sigsigmask[signal];

	old.sa_flags = *pflags;
	old.sa_handler = phandler->handler;
	old.sa_mask = *pmask;

	if (action != NULL) {
		if (new.sa_flags & SA_SIGINFO)
			phandler->sigaction = new.sa_sigaction;
		else
			phandler->handler = new.sa_handler;
		*pflags = new.sa_flags;
		*pmask = new.sa_mask;
	}

	px_release_proctree_mutex(ipl);

	if (oldAction != NULL)
		*oldAction = old;

	return 0;
}

int
psx_sigmask(int how, const sigset_t *__restrict set,
    sigset_t *__restrict retrieve)
{
	ipl_t ipl;
	sigset_t old, new;
	int r = 0;

	if (set != NULL)
		new = *set;

	ipl = px_acquire_proctree_mutex();
	r = psx_domask(how, set != NULL ? &new : NULL, &old);
	px_release_proctree_mutex(ipl);

	if (retrieve != NULL)
		*retrieve = old;

	return r;
}

int
psx_init(void)
{
	int r;

	ke_spinlock_init(&px_proctree_mutex);

	proc_init_common(&posix_proc0, NULL, &posix_thread0, NULL,
	    &kernel_process, ps_curthread());

	r = vfs_mountdev1();
	kassert(r == 0);

	posix_pgroup0.pgid = 0;
	posix_pgroup0.session = &posix_session0;
	RB_INSERT(posix_pgroup_rb, &pgroup_rb, &posix_pgroup0);

	posix_session0.leader = &posix_proc0;
	posix_session0.nmembers = 1;
	posix_session0.sid = 0;

	LIST_INSERT_HEAD(&posix_pgroup0.members, &posix_proc0,
	    pgroup_members_link);
	posix_proc0.pgroup = &posix_pgroup0;

	r = pxp_make_syscon_tty();
	kassert(r == 0);

	kprintf("Launching POSIX init...\n");
	r = psx_fork(NULL, &posix_proc0, &posix_proc1);
	kassert(r == 0);

	return 0;
}

/*! the proctree_mtx is currently locked */
void
psx_signal_proc(posix_proc_t *proc, int sig)
{
	kthread_t *kthread, *chosen = NULL;
	posix_thread_t *thread;
	ipl_t ipl;

	ipl = ke_acquire_dispatcher_lock();

	SLIST_FOREACH (kthread, &proc->eprocess->kproc.threads,
	    kproc_threads_link) {
		thread = px_kthread_to_thread(kthread);
		if (thread->sigmask & sig) {
			continue;
		} else {
			chosen = kthread;
			break;
		}
	}

	if (chosen) {
		thread = px_kthread_to_thread(chosen);

		if (thread->sighandlers[sig].handler == SIG_IGN) {
			kdprintf("signal %d ignored\n", sig);
		} else {
			ksiginfo_t *si = kmem_alloc(sizeof(*si));

			si->siginfo.si_signo = sig;
			TAILQ_INSERT_TAIL(&thread->sigqueue, si, tailq_entry);

			/* if it is waiting interruptibly, cancel its wait */
			if (chosen->state == kThreadStateWaiting) {
				ke_cancel_wait(chosen);
			} else {
				/*
				 * do an IPI if non-local, otherwise proceed
				 * it will be caught as expected
				 */
				kfatal("not implemented yet\n");
			}
		}
	}

	ke_release_dispatcher_lock(ipl);
}

void
psx_signal_pgroup(struct posix_pgroup *pg, int sig)
{
	posix_proc_t *proc;

	LIST_FOREACH (proc, &pg->members, pgroup_members_link) {
		psx_signal_proc(proc, sig);
	}
}

void
pxp_ast(hl_intr_frame_t *frame)
{
	posix_thread_t *thread = px_curthread();

	/*
	 * note: all interrupts are disabled in this context, and as this is the
	 * currently-running thread, we are free to inspect the sigactions of it
	 * as they are
	 */

	if (TAILQ_EMPTY(&thread->sigqueue))
		return;

	ksiginfo_t *ksi = TAILQ_FIRST(&thread->sigqueue);
	TAILQ_REMOVE(&thread->sigqueue, ksi, tailq_entry);

	/* todo: this is platform-specific stuff! move accordingly */
	char *stack = (char *)frame->rsp;
	ucontext_t *uctx;

	/* clear redzone */
	stack += 128;
	/* make space for our uctx */
	stack += sizeof(ucontext_t);
	uctx = (ucontext_t *)stack;

	uctx->uc_sigmask = thread->sigmask;
	uctx->uc_mcontext.gregs[REG_R8] = frame->r8;
	uctx->uc_mcontext.gregs[REG_R9] = frame->r9;
	uctx->uc_mcontext.gregs[REG_R10] = frame->r10;
	uctx->uc_mcontext.gregs[REG_R11] = frame->r11;
	uctx->uc_mcontext.gregs[REG_R12] = frame->r12;
	uctx->uc_mcontext.gregs[REG_R13] = frame->r13;
	uctx->uc_mcontext.gregs[REG_R14] = frame->r14;
	uctx->uc_mcontext.gregs[REG_R15] = frame->r15;

	uctx->uc_mcontext.gregs[REG_RDI] = frame->rdi;
	uctx->uc_mcontext.gregs[REG_RSI] = frame->rsi;
	uctx->uc_mcontext.gregs[REG_RBP] = frame->rbp;
	uctx->uc_mcontext.gregs[REG_RBX] = frame->rbx;
	uctx->uc_mcontext.gregs[REG_RDX] = frame->rdx;
	uctx->uc_mcontext.gregs[REG_RAX] = frame->rax;
	uctx->uc_mcontext.gregs[REG_RCX] = frame->rcx;
	uctx->uc_mcontext.gregs[REG_RSP] = frame->rsp;
	uctx->uc_mcontext.gregs[REG_RIP] = frame->rip;
	uctx->uc_mcontext.gregs[REG_EFL] = frame->rflags;

	frame->rip = px_curproc()->sigentry;
	frame->rdi = ksi->siginfo.si_signo;
	frame->rsi = 0;
	frame->rdx =
	    (uintptr_t)thread->sighandlers[ksi->siginfo.si_signo].handler;
	frame->rcx = thread->sigflags[ksi->siginfo.si_signo] & SA_SIGINFO;
	frame->r8 = (uintptr_t)uctx;
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