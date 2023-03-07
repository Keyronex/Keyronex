/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 24 2023.
 */

#include "ex_private.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "kdk/devmgr.h"

ethread_t init_thread;

/* acpipc/acpipc.cc */
void acpipc_autoconf(void *rsdp);

void
init_thread_start(void *rsdp)
{
	/*! first we maun setup the device tmpfs */
	// setup device tmpfs
	acpipc_autoconf(rsdp);

	vnode_t *vn;
	int r = vfs_lookup(root_vnode, &vn, "/atestdir/hello.txt", 0, NULL);

	kdprintf("lookup of root vnode (%p) yielded result %d vnode %p\n",
	    root_vnode, r, vn);

#if 0
	vm_mdl_t *mdl = vm_mdl_buffer_alloc(1);
	char *buf;

	vm_mdl_map(mdl, (void**)&buf);

	memset(buf, 0x0, PGSIZE);

	iop_t *iop = iop_new_read(root_vnode->vfsp->dev, mdl, 4096, 0);
	iop->stack[0].vnode = vn;
	iop_return_t res = iop_send_sync(iop);
	kdprintf("IOP result: %d\n", res);
	kdprintf("Buf: %s\n", buf);
#endif

#if 1
	char * buf  = kmem_alloc(256);
	memset(buf, 0x0, 256);
	r = vn->ops->read(vn, buf, 255, 0);

	kdprintf("read of /atestdir/hello.txt yielded result %d:\n%s\n", r,
	    buf);
#endif

	/* become some sort of worker thread? */
	for (;;)
		asm("pause");
}

void
ex_init(void *arg)
{
	kdprintf("Executive Init One!\n");
	ps_create_system_thread(&init_thread, "executive initialisation",
	    init_thread_start, arg);
	ki_thread_start(&init_thread.kthread);
	kdprintf("Executive Init Two! SPL %d\n", splget());
}