/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Jul 08 2024.
 */
/*!
 * @file services.c
 * @brief Executive service dispatch.
 */

#include <sys/errno.h>
#include <sys/mman.h>

#include <keyronex/syscall.h>

#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/poll.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "kdk/file.h"
#include "object.h"

/* futex.c */
int krx_futex_wait(int *u_pointer, int expected, nanosecs_t ns);
int krx_futex_wake(int *u_pointer);
/* objman.c */
extern obj_class_t file_class;

int
krx_vm_allocate(size_t size, vaddr_t *out)
{
	int r;
	vaddr_t vaddr;
	r = vm_ps_allocate(ex_curproc()->vm, &vaddr, size, false);
	*out = vaddr;
	return r;
}

int
krx_vm_map(vaddr_t hint, size_t size, int prot, int flags, int handle,
    io_off_t offset, uintptr_t *out)
{
	int r;
	vm_object_t *obj = NULL;
	int protection = kVMRead;

	if (!(flags & MAP_ANON)) {
		struct file *file;
		void *objptr;

		objptr = ex_object_space_lookup(ex_curproc()->objspace, handle);
		if (objptr == NULL)
			return -EBADF;

		file = objptr;
		vn_retain(file->nch.nc->vp);
		obj = file->nch.nc->vp->object;
		obj_release(file);
	} else if (flags & MAP_ANON && flags & MAP_SHARED) {
		kfatal("Implement anon shared\n");
	}

	if (prot & PROT_WRITE)
		protection |= kVMWrite;

	if (prot & PROT_EXEC)
		protection |= kVMExecute;

	r = vm_ps_map_object_view(ex_curproc()->vm, obj, &hint, size, offset,
	    protection, protection, !(flags & MAP_PRIVATE),
	    obj != NULL ? (flags & MAP_PRIVATE) : false, flags & MAP_FIXED);
	if (r == 0)
		*out = hint;

	return r;
}

struct thread_new_info {
	uintptr_t entry;
	uintptr_t stack;
};

static void
user_thread_trampoline(void *arg)
{
	struct thread_new_info info = *(struct thread_new_info *)arg;

	void ki_enter_user_mode(uintptr_t ip, uintptr_t sp);
	kmem_free(arg, sizeof(info));
	ki_enter_user_mode(info.entry, info.stack);
}

int
krx_fork_thread(uintptr_t entry, uintptr_t stack)
{
	kthread_t *thread;
	struct thread_new_info *info = kmem_alloc(sizeof(*info));
	int r;

	info->entry = entry;
	info->stack = stack;

	r = ps_thread_create(&thread, "newthread", user_thread_trampoline, info,
	    ex_curproc());
	if (r != 0) {
		kmem_free(info, sizeof(*info));
		return r;
	}

	ke_thread_resume(thread);

	return thread->tid;
}

static void
krx_thread_exit(void)
{
	ps_exit_this_thread();
}

uintptr_t
ex_syscall_dispatch(enum krx_syscall syscall, uintptr_t arg1, uintptr_t arg2,
    uintptr_t arg3, uintptr_t arg4, uintptr_t arg5, uintptr_t arg6,
    uintptr_t *out1)
{
	switch (syscall) {
	case kKrxDebugMessage: {
		char *msg;
		int r;

		r = copyin_str((const char *)arg1, &msg);
		kassert(r == 0);
		kprintf("[libc]: %s\n", msg);
		kmem_strfree(msg);
		return 0;
	}

	case kKrxTcbSet:
		curthread()->tcb = arg1;
#ifndef __m68k__
		ke_set_tcb(arg1);
#endif
		return 0;

	case kKrxTcbGet:
		return curthread()->tcb;

	case kKrxGetTid:
		return curthread()->tid;

	case kKrxVmAllocate:
		return krx_vm_allocate(arg1, out1);

	case kKrxVmMap:
		return krx_vm_map(arg1, arg2, arg3, arg4, arg5, arg6, out1);

	case kKrxFileOpen:
		return ex_service_file_open(ex_curproc(),
		    (const char *)arg1);

	case kKrxFileClose:
		return ex_service_file_close(ex_curproc(), arg1);

	case kKrxFileReadCached:
		return ex_service_file_read_cached(ex_curproc(), arg1, arg2,
		    arg3);

	case kKrxFileWriteCached:
		return ex_service_file_write_cached(ex_curproc(), arg1, arg2,
		    arg3);

	case kKrxFileSeek:
		return ex_service_file_seek(ex_curproc(), arg1, arg2,
		    arg3);

	case kKrxFileStat:
		return ex_service_file_stat(ex_curproc(), arg1,
		    (const char *)arg2, arg3, (struct stat *)arg4);

	case kKrxFileIoCtl:
		return ex_service_file_ioctl(ex_curproc(), arg1, arg2,
		    (void *)arg3);

	case kKrxEPollCreate:
		return ex_service_epoll_create(ex_curproc(), arg1);

	case kKrxEPollCtl:
		return ex_service_epoll_ctl(ex_curproc(), arg1, arg2, arg3,
		    (struct epoll_event *)arg4);

	case kKrxEPollWait:
		return ex_service_epoll_wait(ex_curproc(), arg1,
		    (struct epoll_event *)arg2, arg3, arg4);

	case kKrxThreadExit:
		krx_thread_exit();

	case kKrxForkThread:
		return krx_fork_thread(arg1, arg2);

	case kKrxFutexWait:
		return krx_futex_wait((int *)arg1, arg2, -1);

	case kKrxFutexWake:
		return krx_futex_wake((int *)arg1);

	default:
		kfatal("unhandled syscall %d\n", syscall);
	}
}
