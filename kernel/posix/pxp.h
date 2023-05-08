/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 02 2023.
 */
/*!
 * @file posix/pasp.h
 * @brief Portable Applications Subsystem private
 */

#ifndef KRX_POSIX_PASP_H
#define KRX_POSIX_PASP_H

#include <sys/signal.h>

#include <abi-bits/mode_t.h>

#include "kdk/kernel.h"
#include "kdk/process.h"

/*
 * Session.
 */
struct posix_session {
	/* (~) Session ID */
	pid_t sid;
	/* (p) Controlling terminal. */
	struct tty *tty;
	/* (p) Number of member pgroups. */
	unsigned nmembers;
	/* (p) Session leader. */
	struct posix_proc *leader;
};

/*!
 * Process group.
 */
struct posix_pgroup {
	RB_ENTRY(posix_pgroup) rb_entry;
	pid_t pgid;
	struct posix_session *session;
	LIST_HEAD(, posix_proc) members;
};

/*!
 * Queued signal.
 */
typedef struct posix_ksiginfo {
	TAILQ_ENTRY(posix_ksiginfo) tailq_entry;
	siginfo_t siginfo;
} ksiginfo_t;

typedef TAILQ_HEAD(ksiginfo_queue, posix_ksiginfo) ksiginfo_queue_t;

/*!
 * posix subsystem counterpart of eprocess_t
 * (p) = proctree_mtx
 * (~) = invariant after initialisation
 */
typedef struct posix_proc {
	/*! (~) corresponding executive process */
	eprocess_t *eprocess;

	/*! (p) parent process */
	struct posix_proc *parent;
	/*! (p) subprocesses */
	LIST_HEAD(, posix_proc) subprocs;
	/*! (p) parent->subprocs linkage */
	LIST_ENTRY(posix_proc) subprocs_link;
	/*! (p) psx_allprocs link */
	TAILQ_ENTRY(posix_proc) allprocs_link;
	/*! (p) process group */
	struct posix_pgroup *pgroup;
	/*! (p) pgroup::members linkage */
	LIST_ENTRY(posix_proc) pgroup_members_link;

	/*! (p) TID counter */
	pid_t tid_counter;

	/*! (p) has it exited? */
	bool exited;
	/*! (p) wait status of an exiting/waited process */
	int wait_stat;
	/*! (p to change state) subprocess state changes */
	kevent_t subproc_state_change;

	/*! signal entry function */
	vaddr_t sigentry;
	/*! (p) process queued signals */
	ksiginfo_queue_t sigqueue;

	/*! File creation mask. */
	mode_t umask;
} posix_proc_t;

/*!
 * posix subsystem counterpart of ethread_t
 *
 * NOTE: Do sighandlers/sigflags need to be locked? For now they are behind
 * proctree_mtx.
 *
 * (~) = invariant after initialisation
 * (p) = proctree_mtx
 */
typedef struct posix_thread {
	pid_t tid;

	union posix_sighandler {
		void (*handler)(int);
		void (*sigaction)(int, siginfo_t *, void *);
	} sighandlers[SIGRTMAX];
	int sigflags[SIGRTMAX];
	sigset_t sigsigmask[SIGRTMAX];
	sigset_t sigmask;

	/*! thread queued signals */
	ksiginfo_queue_t sigqueue;
} posix_thread_t;

#define stringify(x) #x

#if 0
#define px_acquire_proctree_mutex()                                          \
	ke_wait(&px_proctree_mutex, __FILE__ ":" stringify(__LINE__), false, \
	    false, -1)
#define px_release_proctree_mutex() ke_mutex_release(&px_proctree_mutex);
#endif
#define px_acquire_proctree_mutex() ke_spinlock_acquire(&px_proctree_lock)
#define px_release_proctree_mutex(IPL) \
	ke_spinlock_release(&px_proctree_lock, IPL);

static inline posix_proc_t *
px_curproc(void)
{
	posix_proc_t *psx_proc = (posix_proc_t *)ps_curproc()->pas_proc;
	kassert(psx_proc != NULL);
	return psx_proc;
}

static inline posix_thread_t *
px_curthread(void)
{
	posix_thread_t *psx_thread =
	    (posix_thread_t *)ps_curthread()->pas_thread;
	kassert(psx_thread != NULL);
	return psx_thread;
}

static inline posix_thread_t *
px_kthread_to_thread(kthread_t *thread)
{
	ethread_t *ethread = (ethread_t *)thread;
	kassert(ethread->pas_thread != NULL);
	return ethread->pas_thread;
}

struct posix_pgroup *psx_lookup_pgid(pid_t pgid);

#define PSX_GETUMASK() __atomic_load_n(&px_curproc()->umask, __ATOMIC_ACQUIRE)
int sys_exec(posix_proc_t *proc, const char *u_path, const char *u_argp[],
    const char *u_envp[], hl_intr_frame_t *frame);
int psx_fork(hl_intr_frame_t *frame, posix_proc_t *proc, posix_proc_t **out);
pid_t psx_fork_thread(hl_intr_frame_t *frame, void *entry, void *stack);
int psx_exit(int status);
pid_t psx_getpgid(pid_t pid);
pid_t psx_setpgid(pid_t pid, pid_t pgid);
pid_t psx_setsid(void);
pid_t psx_waitpid(pid_t pid, int *status, int flags);
int psx_sigaction(int signal, const struct sigaction *__restrict action,
    struct sigaction *__restrict oldAction);
int psx_sigmask(int how, const sigset_t *__restrict set,
    sigset_t *__restrict retrieve);
int psx_sigsend(pid_t pid, int sig);

/*!
 * @brief Signal a process group.
 *
 * @pre proctree_lock held
 */
void psx_signal_pgroup(struct posix_pgroup *pg, int sig);

void pxp_ast(hl_intr_frame_t *frame);

extern kspinlock_t px_proctree_lock;

#endif /* KRX_POSIX_PASP_H */
