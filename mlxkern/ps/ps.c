/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Tue Feb 14 2023.
 */

#include "hl/hl.h"
#include "ps/ps.h"

eprocess_t kernel_process;
ethread_t kernel_bsp_thread;

void pi_init_0(void)
{
	kernel_process.id = 0;
	kernel_bsp_thread.kthread.process = &kernel_process;
	kernel_bsp_thread.kthread.cpu = hl_curcpu();
}