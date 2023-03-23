/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */


#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/machdep.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kernel/ke_internal.h"
#include "vm/vm_internal.h"

vm_map_t kmap;
eprocess_t kernel_process = { .map = &kmap };
ethread_t kernel_bsp_thread;
uint64_t id = 1;

void
psp_init_0(void)
{
	kernel_process.id = 0;
	kernel_bsp_thread.kthread.process = &kernel_process.kproc;
	kernel_bsp_thread.kthread.cpu = hl_curcpu();
	vm_map_init(kernel_process.map);
}

int
ps_create_system_thread(ethread_t *thread, const char *name,
    void (*start)(void *), void *arg)
{
	return ki_thread_init(&thread->kthread, &kernel_process.kproc, name,
	    start, arg);
}

int
ps_process_create(krx_out eprocess_t **process_out, eprocess_t *parent)
{
	eprocess_t *eproc = kmem_alloc(sizeof(*eproc));
	int r;

	obj_initialise_header(&eproc->kproc.objhdr, kObjTypeProcess);

	eproc->id = id++;

	r = vm_map_fork(parent->map, &eproc->map);
	kassert(r == 0);

	r = ke_process_init(&eproc->kproc);
	kassert(r == 0);

	*process_out = eproc;

	return 0;
}

int
ps_thread_create(krx_out ethread_t **thread_out, eprocess_t *eproc)
{
	ethread_t *ethread = kmem_alloc(sizeof(*ethread));
	int r;

	r = ki_thread_init(&ethread->kthread, &eproc->kproc, NULL, NULL, NULL);
	if (r != 0) {
		kmem_free(ethread, sizeof(*ethread));
		return r;
	}

	*thread_out = ethread;
	return 0;
}