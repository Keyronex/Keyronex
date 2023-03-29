/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 24 2023.
 */

#include <sys/socket.h>

#include <stdint.h>

#include "abi-bits/in.h"
#include "dev/virtio_net.h"
#include "ex_private.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "lwip/tcpip.h"

ethread_t init_thread;

/* acpipc/acpipc.cc */
void acpipc_autoconf(void *rsdp);
/* vm/cleaner.c */
int vm_cleaner_init(void);
/* posix/posixss.c */
int psx_init(void);

static void
tcpip_running(void *unused)
{
	kdprintf("KeySock version 1\n");
}

void
init_thread_start(void *rsdp)
{
	/*! first we maun setup the device tmpfs */
	// setup device tmpfs

	vm_cleaner_init();
	tcpip_init(tcpip_running, NULL);
	acpipc_autoconf(rsdp);

#if 0
	int test_tcpserver(void);
	test_tcpserver();
#endif

	psx_init();

	/* become some sort of worker thread? */
	for (;;)
		asm("pause");
}

void
ex_init(void *arg)
{
	ps_create_system_thread(&init_thread, "executive initialisation",
	    init_thread_start, arg);
	ki_thread_start(&init_thread.kthread);
}