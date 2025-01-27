/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Tue Jan 14 2025.
 */
/*!
 * @file proc.c
 * @brief POSIX process.
 */

#include <kdk/executive.h>
#include <kdk/kern.h>

static kspinlock_t proctree_lock;

typedef struct psx_proc {
	eprocess_t *eprocess;
	uintptr_t pid;
} psx_proc_t;

typedef struct psx_thread {
	kthread_t *kthread;
} psx_thread_t;
