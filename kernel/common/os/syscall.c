/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file syscall.c
 * @brief System call dispatch.
 */

#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/libkern.h>
#include <sys/pcb.h>
#include <sys/proc.h>

#include <keyronex/syscall.h>

uintptr_t
sys_dispatch(karch_trapframe_t *frame, enum posix_syscall syscall,
    uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4,
    uintptr_t arg5, uintptr_t arg6, uintptr_t *out1)
{
	switch (syscall) {
	case SYS_thread_gettid:
		return 0;


	case SYS_tcb_set:
		ke_set_tcb(arg1);
		return 0;

	case SYS_tcb_get:
		return ke_curthread()->tcb;

	case SYS_debug_message: {
		char *msg;
		int len;

		len = strldup_user(&msg, (const char *)arg1, 4095);
		if (len < 0) {
			kdprintf(
			    "libc output failed (couldn't copy message)\n");
			return len;
		}

		kdprintf("[libc (%d %s)]: %s\n", curproc()->comm, msg);
		kmem_free(msg, len + 1);

		return 0;
	}

	default:
		kfatal("Unhandled syscall number %d", syscall);
	}
}
