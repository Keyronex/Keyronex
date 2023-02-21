/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */

#include "kdk/kernel.h"
#include "kdk/machdep.h"
#include "kdk/process.h"
#include "kdk/vmem.h"

eprocess_t kernel_process;
ethread_t kernel_bsp_thread;

static void
init_common(eprocess_t *process)
{
	ke_mutex_init(&process->vmps.mutex);
	RB_INIT(&process->vmps.vad_queue);
	vmp_wsl_init(&process->vmps);
}

void
psp_init_0(void)
{
	kernel_process.id = 0;
	kernel_bsp_thread.kthread.process = &kernel_process;
	kernel_bsp_thread.kthread.cpu = hl_curcpu();
	init_common(&kernel_process);
}
