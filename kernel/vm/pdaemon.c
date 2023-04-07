/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Apr 07 2023.
 */
/*!
 * @file vm/pdaemon.c
 * @brief Page-out daemon
 */

#include "kdk/kernel.h"

static kevent_t vm_lowpages;

void
vm_pdaemon_init(void)
{
	ke_event_init(&vm_lowpages, false);
}

void
vm_pagedaemon(void)
{
	kwaitstatus_t w;

loop:
	w = ke_wait(&vm_lowpages, "vm_pdaemon:vm_lowpages", false, false, -1);
	kassert(w == kKernWaitStatusOK);
	goto loop;
}