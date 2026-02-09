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

#include <keyronex/ktask.h>

typedef struct thread {
	kthread_t kthread;
} thread_t;

typedef struct proc {
	ktask_t ktask;
} proc_t;

#endif /* ECX_KEYRONEX_PROC_H */
