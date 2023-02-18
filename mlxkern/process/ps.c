/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */

#include "kernel/ke.h"
#include "machdep/machdep.h"
#include "vm/kmem.h"
#include "process/ps.h"

eprocess_t kernel_process;
ethread_t kernel_bsp_thread;

static void
init_common(eprocess_t *process)
{
	ke_mutex_init(&process->vmps.mutex);
	RB_INIT(&process->vmps.vad_queue);
	process->vmps.wsl.array_size = 512;
	process->vmps.wsl.max_size = 512;
	process->vmps.wsl.cur_size = 0;
	process->vmps.wsl.entries = kmem_alloc(sizeof(uintptr_t) * 512);
	process->vmps.wsl.head = 0;
	process->vmps.wsl.tail = 0;
}

void
psp_init_0(void)
{
	kernel_process.id = 0;
	kernel_bsp_thread.kthread.process = &kernel_process;
	kernel_bsp_thread.kthread.cpu = hl_curcpu();
	init_common(&kernel_process);
}
