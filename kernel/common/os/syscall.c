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
#include <sys/krx_cred.h>
#include <sys/krx_epoll.h>
#include <sys/krx_file.h>
#include <sys/krx_signal.h>
#include <sys/krx_vfs.h>
#include <sys/libkern.h>
#include <sys/pcb.h>
#include <sys/proc.h>
#include <sys/utsname.h>

#include <keyronex/syscall.h>
#include <time.h>

int
sys_clock_gettime(int clock_id, struct timespec *tp)
{
	kabstime_t now = ke_time();
	tp->tv_sec = now / NS_PER_S;
	tp->tv_nsec = now % NS_PER_S;
	return 0;
}

int
sys_clock_getres(int clock_id, struct timespec *tp)
{
	tp->tv_sec = 0;
	tp->tv_nsec = NS_PER_S / KERN_HZ;
	return 0;
}

static knanosecs_t
ts_to_ns(const struct timespec *ts)
{
	return (knanosecs_t)ts->tv_sec * NS_PER_S + ts->tv_nsec;
}

int
sys_clock_nanosleep(int clock_id, int flags, const struct timespec *rqtp,
    struct timespec *rmtp)
{
	kabstime_t til;
	kevent_t ev;

	kassert(!(flags & ~TIMER_ABSTIME));

	ke_event_init(&ev, false);
	til = ke_time() + ts_to_ns(rqtp);
	kassert(ke_wait1(&ev, "clock_nanosleep", false, til) == -ETIMEDOUT);

	return 0;
}


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

	/*
	 * processes
	 */

	case SYS_exit:
		ktodo();

	case SYS_fork:
		return sys_fork(frame);

	case SYS_execve:
		return sys_execve((const char *)arg1, (char *const *)arg2,
		    (char *const *)arg3);

	case SYS_wait4:
		return sys_wait4((pid_t)arg1, (int *)arg2, (int)arg3,
		    (struct rusage *)arg4);

	case SYS_pdfork:
		ktodo();

	case SYS_pdwait:
		ktodo();

	case SYS_getpid:
		return curproc()->pid;

	case SYS_getppid:
		return sys_getppid(curproc());

	/*
	 * job control
	 */
	case SYS_getpgid:
		return sys_getpgid((pid_t)arg1);

	case SYS_setpgid:
		return sys_setpgid((pid_t)arg1, (pid_t)arg2);

	case SYS_getsid:
		return sys_getsid((pid_t)arg1);

	case SYS_setsid:
		return sys_setsid();

	/*
	 * threads
	 */

	case SYS_thread_gettid:
		return ke_curthread()->tid;

	case SYS_tcb_set:
		ke_set_tcb(arg1);
		return 0;

	case SYS_tcb_get:
		return ke_curthread()->tcb;

	/*
	 * signals
	 */
	case SYS_sigaction:
		return sys_sigaction((int)arg1, (const struct sigaction *)arg2,
		    (struct sigaction *)arg3);

	case SYS_sigentry:
		return 0;

	case SYS_sigprocmask:
		return sys_sigprocmask((int)arg1, (const sigset_t *)arg2,
		    (sigset_t *)arg3);

	case SYS_sigsuspend:
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

	case SYS_ioctl:
		return sys_ioctl(arg1, (unsigned long) arg2, arg3);

	case SYS_fstatat:
		return sys_fstatat(arg1, (const char *)arg2, (int)arg3,
		    (struct stat *)arg4);

	/*
	 * fd manipulation
	 */
	case SYS_pipe:
		ktodo();

	case SYS_dup:
		return sys_dup((int)arg1);

	case SYS_dup2:
		return sys_dup2((int)arg1, (int)arg2);

	case SYS_dup3:
		return sys_dup3((int)arg1, (int)arg2, (int)arg3);

	case SYS_fcntl:
		return sys_fcntl((int)arg1, (int)arg2, (intptr_t)arg3);

	/*
	 * sockets
	 */

	/*
	 * linux affinity
	 */
	case SYS_epoll_create:
		return sys_epoll_create(arg1);

	case SYS_epoll_ctl:
		return sys_epoll_ctl(arg1, arg2, arg3,
		    (struct epoll_event *)arg4);

	case SYS_epoll_wait:
		return sys_epoll_wait(arg1, (struct epoll_event *)arg2, arg3,
		    arg4);

	/*
	 * clock
	 */
	case SYS_clock_gettime:
		return sys_clock_gettime((int)arg1, (struct timespec *)arg2);

	case SYS_clock_getres:
		return sys_clock_getres((int)arg1, (struct timespec *)arg2);

	case SYS_clock_nanosleep:
		return sys_clock_nanosleep((int)arg1, (int)arg2,
		    (const struct timespec *)arg3,
		    (struct timespec *)arg4);


	/*
	 * creds
	 */

	case SYS_getresuid:
		return sys_getresuid((uid_t *)arg1, (uid_t *)arg2,
		    (uid_t *)arg3);

	case SYS_getresgid:
		return sys_getresgid((gid_t *)arg1, (gid_t *)arg2,
		    (gid_t *)arg3);

	case SYS_utsname: {
		struct utsname name = { 0 };
		strcpy(name.sysname, "Keyronex");
		strcpy(name.nodename, "keyronex");
		strcpy(name.release, "0.4");
		strcpy(name.version, __DATE__ " " __TIME__);
		strcpy(name.machine, "?");
		return memcpy_to_user((struct utsname *)arg1, &name,
		   sizeof(struct utsname)) ? -EFAULT : 0;
	}

	default:
		kfatal("Unhandled syscall number %d", syscall);
	}
}
