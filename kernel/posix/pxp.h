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

#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vfs.h"

/*!
 * a posix kernel file descriptor
 */
typedef struct posix_file {
	vnode_t *vnode;
	size_t pos;
} posix_file_t;

/*!
 * a posix process
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

	/*! (p) wait status of an exiting/waited process */
	int wait_stat;
	/*! (p) subprocess state change event */
	kevent_t subproc_state_change;

	/* (p) open files */
	posix_file_t files[64];
} posix_proc_t;

#define stringify(x) #x
#define px_acquire_proctree_mutex()                                          \
	ke_wait(&px_proctree_mutex, __FILE__ ":" stringify(__LINE__), false, \
	    false, -1)
#define px_release_proctree_mutex() ke_mutex_release(&px_proctree_mutex);

static inline posix_proc_t *
px_curproc(void)
{
	posix_proc_t *psx_proc = (posix_proc_t *)ps_curproc()->pas_proc;
	kassert(psx_proc != NULL);
	return psx_proc;
}

int sys_exec(posix_proc_t *proc, const char *u_path, const char *u_argp[],
    const char *u_envp[], hl_intr_frame_t *frame);

extern kmutex_t px_proctree_mutex;

#endif /* KRX_POSIX_PASP_H */
