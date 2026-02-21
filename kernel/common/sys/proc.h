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
#include <sys/pcb.h>

typedef struct thread {
	kthread_t kthread;
	struct vm_map *vm_map;
} thread_t;

typedef struct proc {
	ktask_t ktask;
	char comm[32];
	struct vm_map *vm_map;
	struct uf_info *finfo;
	pid_t pid;
} proc_t;

proc_t *proc_create(proc_t *parent, bool fork);

thread_t *proc_new_thread(proc_t *proc, karch_trapframe_t *fork_frame,
    void (*func)(void *), void *arg);
thread_t *proc_new_system_thread(void (*func)(void*), void *arg);

void proc_init(void);

extern proc_t proc0;

#define thread_proc(THR) ((proc_t *)((THR)->kthread.task))
#define kthread_proc(KTHR) ((proc_t *)((KTHR)->task))
#define thread_vm_map(THR) \
	((THR)->vm_map != NULL ? (THR)->vm_map : thread_proc(THR)->vm_map)

#define curthread() ((thread_t *)ke_curthread())
#define curproc() thread_proc(curthread())

#endif /* ECX_KEYRONEX_PROC_H */
