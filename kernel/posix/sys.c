/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#include <sys/poll.h>

#include "abi-bits/errno.h"
#include "abi-bits/fcntl.h"
#include "abi-bits/seek-whence.h"
#include "abi-bits/stat.h"
#include "abi-bits/vm-flags.h"
#include "amd64.h"
#include "executive/epoll.h"
#include "kdk/amd64/mdamd64.h"
#include "kdk/amd64/portio.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/machdep.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/posixss.h"
#include "kdk/process.h"
#include "kdk/vm.h"
#include "posix/pxp.h"

void *
vm_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
	vaddr_t at_addr = (vaddr_t)addr;
	eprocess_t *proc = ps_curproc();
	uintptr_t r;

#if DEBUG_SYSCALLS == 1
	kdprintf(
	    " !!! VM_POSIX: mmap addr %p, len %lu, prot %d, flags %d, fd %d, "
	    "offs %ld\n",
	    addr, len, prot, flags, fd, offset);
#endif

	if (flags & MAP_FIXED && PGROUNDDOWN(addr) != (uintptr_t)addr)
		return (void *)-EINVAL; /* must be page-aligned */
	else if (PGROUNDDOWN(offset) != offset)
		return (void *)-EINVAL;

	if (!(flags & MAP_ANON)) {
		struct file *file;
		vm_protection_t vmprot = 0;
		bool private = flags & MAP_PRIVATE;

		file = ps_getfile(proc, fd);
		if (!file)
			return (void *)-EBADF;

		if (private)
			kassert(!(flags & MAP_SHARED));

		vmprot = prot & PROT_WRITE ? kVMAll : (kVMRead | kVMExecute);

		/* TODO(low): introduce a vnode mmap operation (for devices) */

		r = vm_map_object(proc->map, &file->vn->vmobj, &at_addr, len,
		    offset, vmprot, kVMAll,
		    private ? kVMInheritCopy : kVMInheritShared,
		    flags & MAP_FIXED, private);
	} else {
		r = vm_map_allocate(proc->map, &at_addr, len,
		    flags & MAP_FIXED);
	}

#if 0
	kdprintf("Actual addr: 0x%lx\n", at_addr);
#endif

	return (r == 0) ? (void *)at_addr : (void *)r;
}

int
posix_do_openat(vnode_t *dvn, const char *path, int mode)
{
	eprocess_t *eproc = ps_curproc();
	vnode_t *vn;
	int r;
	int fd = -1;
	kwaitstatus_t w;

	w = ke_wait(&eproc->fd_mutex, "sys_open:eproc->fd_mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	for (int i = 0; i < elementsof(eproc->files); i++) {
		if (eproc->files[i] == NULL) {
			fd = i;
			break;
		}
	}

#if DEBUG_SYSCALLS == 1
	kprintf("PID %d sys_open(%s,%d) to FD %d\n", proc->kproc->pid, path,
	    mode, fd);
#endif

	if (fd == -1)
		return -ENFILE;

	r = vfs_lookup(dvn, &vn, path, 0, NULL);
	if (r < 0 && mode & O_CREAT)
		r = vfs_lookup(dvn, &vn, path, kLookupCreate, NULL);

	if (r < 0) {
#if DEBUG_SYSCALLS == 1
		kprintf("lookup returned %d\n", r);
#endif
		goto out;
	}

	if (vn->ops->open) {
		r = VOP_OPEN(&vn, mode);
		if (r < 0) {
#if DEBUG_SYSCALLS == 1
			kprintf("open returned %d\n", r);
#endif
			goto out;
		}
	}

	eproc->files[fd] = kmem_alloc(sizeof(struct file));
	obj_initialise_header(&eproc->files[fd]->objhdr, kObjTypeFile);
	eproc->files[fd]->vn = vn;
	eproc->files[fd]->offset = 0;

	r = fd;

out:
	ke_mutex_release(&eproc->fd_mutex);
	return r;
}

int
sys_open(const char *path, int mode)
{
	return posix_do_openat(root_vnode, path, mode);
}

int
sys_close(int fd)
{
	eprocess_t *eproc = ps_curproc();
	kwaitstatus_t w;
	int r = 0;

	if (fd < 0 || fd >= 64)
		return -EBADF;

	w = ke_wait(&eproc->fd_mutex, "sys_open:eproc->fd_mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);

	if (eproc->files[fd] == NULL) {
		r = -EBADF;
	} else {
		obj_direct_release(eproc->files[fd]);
		eproc->files[fd] = NULL;
	}

	ke_mutex_release(&eproc->fd_mutex);

	return r;
}

int
sys_read(int fd, void *buf, size_t nbyte)
{
	struct file *file = ps_getfile(ps_curproc(), fd);
	int r;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_READ(%d, nbytes: %lu off: %lu)\n", fd, nbyte,
	    file->offset);
#endif

	if (file == NULL)
		return -EBADF;

	r = VOP_READ(file->vn, buf, nbyte, file->offset);
	if (r < 0) {
		kdprintf("VOP_READ got %d\n", r);
		return r;
	}

	file->offset += r;

	return r;
}

int
sys_write(int fd, void *buf, size_t nbyte)
{
	struct file *file = ps_getfile(ps_curproc(), fd);
	int r;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_WRITE(%d, nbytes: %lu off: %lu)\n", fd, nbyte,
	    file->offset);
#endif

	if (file == NULL)
		return -EBADF;

	r = VOP_WRITE(file->vn, buf, nbyte, file->offset);
	if (r < 0) {
		kdprintf("VOP_WRITE got %d\n", r);
		return r;
	}

	file->offset += r;

	return r;
}

int
sys_seek(int fd, off_t offset, int whence)
{
	struct file *file = ps_getfile(ps_curproc(), fd);

#if DEBUG_SYSCALLS == 1
	kprintf("SYS_SEEK(offset: %ld)\n", offset);
#endif

	if (file == NULL)
		return -EBADF;

	if (file->vn->type != VREG)
		return -ESPIPE;

	if (whence == SEEK_SET)
		file->offset = offset;
	else if (whence == SEEK_CUR)
		file->offset += offset;
	else if (whence == SEEK_END) {
		vattr_t attr;
		int r;

		r = file->vn->ops->getattr(file->vn, &attr);
		if (r < 0)
			return r;

		file->offset = attr.size + offset;
	}

	return file->offset;
}

int
sys_stat(enum posix_stat_kind kind, int fd, const char *path, int flags,
    struct stat *sb)
{
	int r;
	vnode_t *vn;
	vattr_t vattr;
	char *pathcpy = NULL;

	if (kind == kPXStatKindAt) {
		struct file *file = ps_getfile(ps_curproc(), fd);

		pathcpy = strdup(path);

		if (file == NULL) {
			r = -EBADF;
			goto out;
		}

		r = vfs_lookup(file->vn, &vn, pathcpy, 0, NULL);
		if (r != 0)
			goto out;
	} else if (kind == kPXStatKindCWD) {
		pathcpy = strdup(path);

		r = vfs_lookup(root_vnode, &vn, pathcpy, 0, NULL);
		if (r != 0)
			goto out;
	} else {
		kassert(kind == kPXStatKindFD);

		struct file *file = ps_getfile(ps_curproc(), fd);

		if (file == NULL)
			return -EBADF;

		vn = file->vn;
		obj_direct_retain(vn);
	}

	r = VOP_GETATTR(vn, &vattr);
	if (r != 0) {
		obj_direct_release(vn);
		goto out;
	}

	memset(sb, 0x0, sizeof(*sb));
	sb->st_mode = vattr.mode;

	switch (vattr.type) {
	case VREG:
		sb->st_mode |= S_IFREG;
		break;

	case VDIR:
		sb->st_mode |= S_IFDIR;
		break;

	case VCHR:
		sb->st_mode |= S_IFCHR;
		break;

	case VLNK:
		sb->st_mode |= S_IFLNK;
		break;

	case VSOCK:
		sb->st_mode |= S_IFSOCK;
		break;

	case VNON:
		kfatal("Should be unreachable!\n");
	}

	sb->st_size = vattr.size;
	sb->st_blocks = vattr.size / 512;
	sb->st_blksize = 512;

out:
	if (pathcpy != NULL)
		kmem_strfree(pathcpy);
	return r;
}

uintptr_t
sys_ppoll(struct pollfd *pfds, int nfds, const struct timespec *timeout,
    const sigset_t *sigmask)
{
	int r;
	struct epoll *epoll;

	kassert(nfds > 0 && nfds < 25);

	epoll = epoll_new();

	for (size_t i = 0; i < nfds; i++) {
		struct epoll_event ev;
		struct pollfd *pfd = &pfds[i];

		ev.data.u32 = i;
		ev.events = 0;
		if (pfd->events & POLLIN)
			ev.events |= EPOLLIN;
		if (pfd->events & POLLOUT)
			ev.events |= EPOLLOUT;

		r = epoll_do_ctl(epoll, EPOLL_CTL_ADD, pfd->fd, &ev);
		if (r != 0) {
			epoll_do_destroy(epoll);
			return r;
		}
	}

	struct epoll_event *revents = NULL;
	revents = kmem_alloc(sizeof(struct epoll_event) * nfds);

	nanosecs_t nanosecs;
	if (!timeout)
		nanosecs = -1;
	else if (timeout->tv_nsec <= 100000 && timeout->tv_nsec == 0)
		nanosecs = 0;
	else
		nanosecs = (nanosecs_t)timeout->tv_sec * NS_PER_S +
		    (nanosecs_t)timeout->tv_nsec;

	r = epoll_do_wait(epoll, revents, nfds, nanosecs);
	kassert(r >= 0);
	epoll_do_destroy(epoll);

	for (int i = 0; i < nfds; i++)
		pfds[i].revents = 0;

	for (int i = 0; i < r; i++) {
		/* todo: that's not right */
		pfds[revents[i].data.u32].revents = revents[i].events;
	}

	return r;
}

uintptr_t
sys_fork(hl_intr_frame_t *frame)
{
	struct posix_proc *parent, *child;
	int r;

	parent = px_curproc();
	r = psx_fork(frame, parent, &child);

	if (r != 0) {
		kassert(r < 0);
		return r;
	} else {
		return child->eprocess->id;
	}
}

int
posix_syscall(hl_intr_frame_t *frame)
{
#define ARG1 frame->rdi
#define ARG2 frame->rsi
#define ARG3 frame->rdx
#define ARG4 frame->r10
#define ARG5 frame->r8
#define ARG6 frame->r9

#define RET frame->rax
#define OUT frame->rdi

#if 0
	kdprintf("Syscall %lu\n", frame->rax);
#endif

	switch (frame->rax) {
	case kPXSysDebug:
		kdprintf("<DEBUG>: %s\n", (char *)ARG1);
		syscon_printstats();
		break;

	case kPXSysMmap:
		RET = (uintptr_t)vm_mmap((void *)ARG1, ARG2, ARG3, ARG4, ARG5,
		    ARG6);
		break;

	/* file syscalls */
	case kPXSysOpen:
		RET = sys_open((const char *)ARG1, ARG2);
		break;

	case kPXSysClose:
		RET = sys_close(ARG1);
		break;

	case kPXSysRead:
		RET = sys_read(ARG1, (void *)ARG2, ARG3);
		break;

	case kPXSysWrite:
		RET = sys_write(ARG1, (void *)ARG2, ARG3);
		break;

	case kPXSysSeek:
		RET = sys_seek(ARG1, ARG2, ARG3);
		break;

	case kPXSysStat:
		RET = sys_stat(ARG1, ARG2, (const char *)ARG3, ARG4,
		    (struct stat *)ARG5);
		break;

	case kPXSysPPoll:
		RET = sys_ppoll((struct pollfd *)ARG1, ARG2,
		    (const struct timespec *)ARG3, (const sigset_t *)ARG4);
		break;

	/* process & misc misc */
	case kPXSysFork:
		RET = sys_fork(frame);
		break;

	case kPXSysExecVE:
		RET = sys_exec(px_curproc(), (char *)ARG1, (const char **)ARG2,
		    (const char **)ARG3, frame);
		break;

	case kPXSysSetFSBase:
		ke_curthread()->hl.fs = ARG1;
		wrmsr(kAMD64MSRFSBase, ARG1);
		RET = 0;
		break;

	case kPXSysGetPID:
		RET = px_curproc()->eprocess->id;
		break;

	case kPXSysGetPPID:
		RET = px_curproc()->parent->eprocess->id;
		break;

	default:
		kfatal("Unknown syscall %lu\n", frame->rax);
	}

	return true;
}