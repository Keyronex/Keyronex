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

#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vfs.h"

/*
 * Session.
 */
struct posix_session {
	/* (~) Session ID */
	pid_t sid;
	/* (p) Number of members. */
	unsigned nmembers;
	/* (p) Session leader. */
	struct posix_proc *leader;
};

/*!
 * Process group.
 */
struct posix_pgroup {
	pid_t pgid;
	struct session *session;
	LIST_HEAD(, posix_proc) members;
};

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

	/*! (p) has it exited? */
	bool exited;
	/*! (p) wait status of an exiting/waited process */
	int wait_stat;
	/*! (p to change state) subprocess state changes */
	kevent_t subproc_state_change;

	/*! signal entry function */
	vaddr_t sigentry;
} posix_proc_t;

struct px_sigaction {
	union {
		void (*handler)(int);
		void (*sigaction)(int, siginfo_t *, void *);
	} handler;
	int sa_flags;
};

/*!
 * posix subsystem counterpart of ethread_t
 *
 * (~) = invariant after initialisation
 * (p) = proctree_mtx
 */
typedef struct posix_thread {
	struct px_sigaction sigactions[SIGRTMAX];
	sigset_t sigmask;
} posix_thread_t;

#define stringify(x) #x

#if 0
#define px_acquire_proctree_mutex()                                          \
	ke_wait(&px_proctree_mutex, __FILE__ ":" stringify(__LINE__), false, \
	    false, -1)
#define px_release_proctree_mutex() ke_mutex_release(&px_proctree_mutex);
#endif
#define px_acquire_proctree_mutex() ke_spinlock_acquire(&px_proctree_mutex)
#define px_release_proctree_mutex(IPL) \
	ke_spinlock_release(&px_proctree_mutex, IPL);

static inline posix_proc_t *
px_curproc(void)
{
	posix_proc_t *psx_proc = (posix_proc_t *)ps_curproc()->pas_proc;
	kassert(psx_proc != NULL);
	return psx_proc;
}

int sys_exec(posix_proc_t *proc, const char *u_path, const char *u_argp[],
    const char *u_envp[], hl_intr_frame_t *frame);
int psx_fork(hl_intr_frame_t *frame, posix_proc_t *proc, posix_proc_t **out);
int psx_exit(int status);
pid_t psx_waitpid(pid_t pid, int *status, int flags);

extern kspinlock_t px_proctree_mutex;

#endif /* KRX_POSIX_PASP_H */
