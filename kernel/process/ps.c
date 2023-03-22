/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 14 2023.
 */

#include "kdk/kernel.h"
#include "kdk/machdep.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "kdk/vmem.h"
#include "kernel/ke_internal.h"
#include "vm/vm_internal.h"

vm_map_t kmap;
eprocess_t kernel_process = {.map = &kmap};
ethread_t kernel_bsp_thread;

static void
init_common(eprocess_t *process)
{
	ke_mutex_init(&process->map->mutex);
	RB_INIT(&process->map->entry_queue);
}

void
psp_init_0(void)
{
	kernel_process.id = 0;
	kernel_bsp_thread.kthread.process = &kernel_process.kproc;
	kernel_bsp_thread.kthread.cpu = hl_curcpu();
	init_common(&kernel_process);
}

int ps_create_system_thread(ethread_t *thread, const char *name, void (*start)(void*), void *arg)
{
	return ki_thread_init(&thread->kthread, &kernel_process.kproc, name,  start, arg);
}