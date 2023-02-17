/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 16 2023.
 */

#include "kernel/ke_internal.h"

void
kmd_thread_init(struct kthread *thread, void (*start_fun)(void *),
    void		      *start_arg)
{
	thread->frame.cs = 0x28;
	thread->frame.ss = 0x30;
	thread->frame.rflags = 0x202;
	thread->frame.rip = (uintptr_t)start_fun;
	thread->frame.rdi = (uintptr_t)start_arg;
	thread->frame.rbp = 0;
	/*
	 * subtract 8 since there is no `call` prior to executing the function
	 * so its push %rbp will misalign the stack
	 */
	thread->frame.rsp = (uintptr_t)thread->kstack - 8;
}
