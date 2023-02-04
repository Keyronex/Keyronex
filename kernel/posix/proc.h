#ifndef PROC_H_
#define PROC_H_

#include <vfs/vfs.h>

typedef struct session {
	/*! controlling terminal vnode */
	vnode_t *tty;
} session_t;

typedef struct pgrp {
	/*! members of pgrp */
	LIST_HEAD(, proc) members;
	/*! session to which it belongs */
	session_t *session;
} pgrp_t;

/*!
 * a posix process
 * (l) = proc::lock
 * (p) = proc_lock
 * (~) = invariant
 */
typedef struct proc {
	kmutex_t lock;

	/*! (~) corresponding kprocess */
	kprocess_t *kproc;

	/*! (p) posix state */
	enum {
		kPASProcNormal,
		kPASProcCompleted,
	} state;
	/*! if completed, its wait status */
	int wstat;

	/*! (p) parent process */
	struct proc *parent;
	/*! (p) subprocesses */
	LIST_HEAD(, proc) subprocs;
	/*! (p) parent->subprocs linkage */
	LIST_ENTRY(proc) subprocs_link;
	/*! (p) process group */
	pgrp_t *pgrp;
	/*! (p) pgrp->members linkage */
	LIST_ENTRY(proc) pgrp_members_link;

	/*! (l) open files */
	file_t *files[64];

	/*!
	 * (p to use) subprocess state change event (used for wait* APIs)
	 * written to by immediate subprocesses when their state changes.
	 */
	kevent_t statechange;
} proc_t;

/*!
 * Process state lock.
 */
extern kmutex_t proc_lock;

/*!
 * Common initialisation of a process shared between forking and creation of
 * the initial proc0. (Does not set the kprocess pointer.)
 */
void procx_init(proc_t *proc, proc_t *super) LOCK_REQUIRES(proc_lock);

/*!
 * Fork the current process. No locking expected on entry.
 *
 * @param errp pointer to where an error, if it occurs, will be written.
 *
 * @retval NULL fork failed; error noted in errp.
 * @retval otherwise fork succeeded, new process returned
 */
proc_t *proc_fork(uintptr_t *errp);

int sys_exec(proc_t *proc, const char *u_path, const char *u_argp[],
    const char *u_envp[], md_intr_frame_t *frame);

#endif /* PROC_H_ */
