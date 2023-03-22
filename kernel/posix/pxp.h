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
 * (p) = posix_proc_mtx
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

#endif /* KRX_POSIX_PASP_H */
