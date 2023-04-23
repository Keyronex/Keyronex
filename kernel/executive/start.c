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
/* vm/pdaemon.c */
void vm_pagedaemon(void);
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
	int r;

	r = vfs_mountdev1();
	kassert(r == 0);

	vm_cleaner_init();
	tcpip_init(tcpip_running, NULL);
	acpipc_autoconf(rsdp);

#if 0
	int test_tcpserver(void);
	test_tcpserver();
#endif

	kassert(root_vnode != 0);
	vnode_t *dev_shadowed;

	r = vfs_lookup(root_vnode, &dev_shadowed, "dev", 0, NULL);
	kassert(r == 0);

	dev_shadowed->vfsmountedhere = &dev_vfs;

	int fbdev_init(void);
	fbdev_init();

	psx_init();

	/* become some sort of worker thread? */
	vm_pagedaemon();
}

void
ex_init(void *arg)
{
	ps_create_system_thread(&init_thread, "executive initialisation",
	    init_thread_start, arg);
	ki_thread_start(&init_thread.kthread);
}

void
file_free(struct file *file)
{
	if (file->vn->ops->close)
		file->vn->ops->close(file->vn);
}