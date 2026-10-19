/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file proc.h
 * @brief POSIX process & thread.
 */

#ifndef ECX_KEYRONEX_PROC_H
#define ECX_KEYRONEX_PROC_H

#include <sys/types.h>
#include <sys/k_thread.h>
#include <sys/k_wait.h>
#include <sys/pcb.h>
#include <stdbool.h>

struct rusage;

typedef struct thread {
	kthread_t kthread;
	struct vm_map *vm_map;
} thread_t;

/* all under the proctree_lock */
struct session {
	uint32_t refcount;
	pid_t sid;
	struct proc *leader;
	struct vnode *ctty_vn;
	struct stdata *ctty_str;
	LIST_HEAD(, pgrp) pgrps;
};

/* all under the proctree_lock */
struct pgrp {
	uint32_t refcount;
	pid_t pgid;
	LIST_ENTRY(pgrp) session_link;
	LIST_HEAD(, proc) members;
	struct session *session;
};

/*
 * Process lifecycle state
 *
 * RUNNING: new threads may be created
 * EXECING: a thread is in execve() and draining other threads.
 * EXITING: process is dying, last one to go zombifies the process.
 *
 */
enum proc_lifecycle {
	PROC_RUNNING,
	PROC_EXECING,
	PROC_EXITING,
};

/*
 * ~: invariant from creation
 * T: proctree_mutex
 * t: proc->ktask.threads_lock
 */
typedef struct proc {
	ktask_t	ktask;

	pid_t		pid;		/* ~: process ID */
	struct pgrp	*pgrp;		/* T: pgrp this process is in */
	LIST_ENTRY(proc) pgrp_link;	/* T: link in pgrp::members */
	struct procdesc	*procdesc;	/* T: procdesc referring to this proc */

	TAILQ_ENTRY(proc)	allproc_qlink;	/* T@ link in allproc */
	struct proc 		*parent;	/* T: parent process pointer */
	TAILQ_HEAD(, proc)	children;	/* T: child processes */
	TAILQ_ENTRY(proc)	sibling_qlink;	/* T: links parent's children */
	bool		exited;	 	/* T: process exited, needs wait()'d */
	enum proc_lifecycle lifecycle;	/* t */
	int		exit_status; 	/* t: set in EXITING
					 * read after `exited` */
	kevent_t	child_ev;	/* T: set whenever a child exits */
	kevent_t	thread_ev;	/* t: set when thread exits in EXECING */

	char comm[32];			/* belongs in user */
	struct vm_map *vm_map;		/* ~? virtual memory map */
	struct uf_info *finfo;		/* belongs in user*/

	kmutex_t flock_mutex;		/* protects flock_entries */
	TAILQ_HEAD(, flock_entry) flocks_held; /* file locks held */
} proc_t;


void proc_init(void);
void proc_reaper_init(void);

/* drain sibling threads after execve() commits; -EINTR if proc should exit */
int proc_exec_begin(proc_t *proc);
/* destroy a process that never run (i.e. fork() failed) */
void proc_destroy_unstarted(proc_t *proc);

proc_t *proc_curproc(void);

proc_t *proc_create(proc_t *parent, bool fork);

thread_t *proc_new_thread(proc_t *proc, karch_trapframe_t *fork_frame,
    void (*func)(void *), void *arg);
thread_t *proc_new_system_thread(void (*func)(void*), void *arg);

struct session *session_alloc(pid_t sid, struct proc *leader);
void session_ref(struct session *sess);
void session_unref(struct session *sess);

struct pgrp *pgrp_alloc(pid_t pgid, struct session *sess);
void pgrp_ref(struct pgrp *pgrp);
void pgrp_unref(struct pgrp *pgrp);
void pgrp_add_member(struct pgrp *pgrp, struct proc *proc);
void pgrp_remove_member(struct proc *proc);

void proc_reap_locked(proc_t *parent, proc_t *child);

void procdesc_notify_exit(struct procdesc *pd, int exit_status);
void procdesc_orphan(struct procdesc *pd);

/* (move me) strhead.c detach tty from its session (session leader exiting) */
void str_tty_detach_session_locked(struct stdata *sh);

int sys_fork_thread(uintptr_t stack, uintptr_t entry);
void sys_thread_exit(void);
void sys_exit(int status);
pid_t sys_wait4(pid_t pid, int *status, int flags, struct rusage *ru);
pid_t sys_fork(karch_trapframe_t *frame);
int sys_execve(const char *upath, char *const uarpg[], char *const uenvp[]);
pid_t sys_pdfork(karch_trapframe_t *frame, int *fdp, int flags);
pid_t sys_pdwait(int pd, int *status, int options, struct rusage *ru,
    siginfo_t *si);

pid_t sys_getppid(proc_t *proc);
pid_t sys_getpgid(pid_t pid);
pid_t sys_getsid(pid_t pid);
int sys_setpgid(pid_t pid, pid_t pgid);
pid_t sys_setsid(void);

extern proc_t proc0;
extern proc_t *initproc;

#define thread_proc(THR) ((proc_t *)((THR)->kthread.task))
#define kthread_proc(KTHR) ((proc_t *)((KTHR)->task))
#define thread_vm_map(THR) \
	((THR)->vm_map != NULL ? (THR)->vm_map : thread_proc(THR)->vm_map)

#define curthread() ((thread_t *)ke_curthread())
#define curproc() thread_proc(curthread())

#endif /* ECX_KEYRONEX_PROC_H */
