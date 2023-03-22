/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Feb 11 2023.
 */

#ifndef KRX_KDK_PROCESS_H
#define KRX_KDK_PROCESS_H

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
 *
 * (~) => invariant after initialisation
 */
typedef struct eprocess {
	/*! Kernel process part. */
	kprocess_t kproc;
	/*! (~) Unique process identifier. */
	uint32_t id;
	/*! Virtual memory state. */
	vm_map_t *map;
	/*! (~) Portable Applications Subsystem process. */
	void *pas_proc;

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
int ps_create_system_thread(ethread_t *thread, const char *name,
    void (*start)(void *), void *arg);

extern eprocess_t kernel_process;
extern ethread_t kernel_bsp_thread;

#endif /* KRX_KDK_PROCESS_H */
