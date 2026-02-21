/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file syscall.c
 * @brief System call dispatch.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/krx_file.h>
#include <sys/krx_vfs.h>
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
	case SYS_debug_message: {
		char *msg;
		int len;

		len = strldup_user(&msg, (const char *)arg1, 4095);
		if (len < 0) {
			kdprintf(
			    "libc output failed (couldn't copy message)\n");
			return len;
		}

		kdprintf("[libc]: %s\n", msg);
		kmem_free(msg, len + 1);

		return 0;
	}

	case SYS_thread_gettid:
		return 0;

	case SYS_tcb_set:
		ke_set_tcb(arg1);
		return 0;

	case SYS_tcb_get:
		return ke_curthread()->tcb;

	/*
	 * signals
	 */
	case SYS_sigaction:
		return -ENOSYS;

	case SYS_sigentry:
		return -ENOSYS;

	/*
	 * vm
	 */

#if 1
	case SYS_mmap:
		return (uintptr_t)sys_mmap((void *)arg1, arg2, arg3, arg4, arg5,
		    arg6);
#endif

	case SYS_munmap:
		return 0;

	/*
	 * vfs ops
	 */

	case SYS_openat:
		return sys_openat(arg1, (const char *)arg2, arg3, arg4);

	/*
	 *  file ops
	 */

	case SYS_close:
		return sys_close((int)arg1);

	case SYS_read:
		return sys_read(arg1, (void *)arg2, arg3);

	case SYS_write:
		return sys_write(arg1, (const void *)arg2, arg3);

	case SYS_seek:
		return sys_lseek(arg1, arg2, (int)arg3, (off_t *)out1);

	default:
		kfatal("Unhandled syscall number %d", syscall);
	}
}
