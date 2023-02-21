/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Sat Feb 11 2023.
 */

#ifndef MLX_PS_PS_H
#define MLX_PS_PS_H

#include "./kernel.h"
#include "./object.h"
#include "./vm.h"

/*!
 * An executive thread.
 */
typedef struct ethread {
	/*! Kernel thread. */
	kthread_t kthread;
} ethread_t;

/*!
 * An executive process
 */
typedef struct eprocess {
	/*! Kernel process part. */
	kprocess_t kproc;
	/*! Unique process identifier. */
	uint32_t id;
	/*! Virtual memory state. */
	vm_procstate_t vmps;
} eprocess_t;

/*! Eternal handle to the kernel process. Only useable by  */
#define kernel_process_handle (handle_t)(-1)

/*! Get the currently-running process. */
#define ps_curproc() ((eprocess_t *)ke_curthread()->process)

/*!
 * Create a new thread of a kernel process.
 *
 * @param thread Pointer to an uninitialised ethread_t structure which will be
 * initialised as the thread.
 *
 * @post New thread is created, with one reference held to it.
 */
int ps_create_system_thread(ethread_t *thread);

extern eprocess_t kernel_process;
extern ethread_t kernel_bsp_thread;

#endif /* MLX_PS_PS_H */
