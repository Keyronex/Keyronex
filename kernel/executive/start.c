/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 24 2023.
 */

#include "ex_private.h"
#include "kdk/kernel.h"
#include "kdk/process.h"

ethread_t init_thread;

/* acpipc/acpipc.cc */
void acpipc_autoconf(void *rsdp);

void
init_thread_start(void *rsdp)
{
	/*! first we maun setup the device tmpfs */

	acpipc_autoconf(rsdp);

	/* become some sort of worker thread? */
	for (;;)
		asm("pause");
}

void ex_init(void *arg) {
	kdprintf("Executive Init One!\n");
	ps_create_system_thread(&init_thread, "executive initialisation", init_thread_start, arg);
	ki_thread_start(&init_thread.kthread);
	kdprintf("Executive Init Two! SPL %d\n", splget());
}