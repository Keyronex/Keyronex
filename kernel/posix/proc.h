#ifndef POSIX_PROC_H_
#define POSIX_PROC_H_

#include <vfs/vfs.h>

/*!
 * a posix process
 * (p) = proc::lock
 * (~) = invariant
 */
typedef struct proc {
	kmutex_t lock;

	/*! (~) corresponding kprocess */
	kprocess_t *kproc;
	/*! (p) parent process */
	struct proc *parent;
	/*! (p) subprocesses */
	LIST_HEAD(, proc) subprocs;
	/*! (p) subprocesses linkage */
	LIST_ENTRY(proc) subprocs_link;

	/*! (p) open files */
	file_t *files[64];
} proc_t;

#endif /* POSIX_PROC_H_ */
