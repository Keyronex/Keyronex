/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#include <sys/poll.h>

#include <abi-bits/utsname.h>
#include <dirent.h>
#include <keyronex/syscall.h>

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
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "posix/pxp.h"

/* devmgr/fifofs.c */
int sys_pipe(int *out, int flags);
/* executive/?.c */
int sys_dup3(int oldfd, int newfd, int flags);

#if 0
#define DEBUG_SYSCALLS 1
#endif

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

	len = PGROUNDUP(len);

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
sys_ioctl(int fd, unsigned long command, void *data)
{
	struct file *file = ps_getfile(ps_curproc(), fd);

#if DEBUG_SYSCALLS == 0
	kdprintf("SYS_IOCTL(fd: %d, command: 0x%lx\n", fd, command);
#endif

	if (file == NULL)
		return -EBADF;

	kassert(file->vn->ops->ioctl != NULL);
	return VOP_IOCTL(file->vn, command, data);
}

int
posix_do_openat(vnode_t *dvn, const char *path, int flags, int mode)
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
	kdprintf("sys_open(%s,%d) to FD %d\n", path, mode, fd);
#endif

	if (fd == -1)
		return -ENFILE;

	r = vfs_lookup(dvn, &vn, path, 0, NULL);
	if (r < 0 && flags & O_CREAT) {
		vattr_t attr;
		attr.mode = S_IFREG | 0755;
		attr.type = VREG;
		r = vfs_lookup(dvn, &vn, path, kLookupCreate, &attr);
	}

	if (r < 0) {
#if DEBUG_SYSCALLS == 1
		kdprintf("lookup returned %d\n", r);
#endif
		goto out;
	}

	if (vn->ops->open) {
		r = VOP_OPEN(&vn, mode);
		if (r < 0) {
#if DEBUG_SYSCALLS == 1
			kdprintf("open returned %d\n", r);
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
sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	vnode_t *dvn;

	if (dirfd == AT_FDCWD) {
		dvn = root_vnode;
	} else {
		kfatal("Unimplemented\n");
	}

	return posix_do_openat(dvn, path, flags, mode);
}

int
do_close(eprocess_t *eproc, int fd)
{
	if (eproc->files[fd] == NULL) {
		return -EBADF;
	} else {
		obj_direct_release(eproc->files[fd]);
		eproc->files[fd] = NULL;
		return 0;
	}
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

	r = do_close(eproc, fd);

	ke_mutex_release(&eproc->fd_mutex);

	return r;
}

int
sys_dup(int oldfd)
{
	eprocess_t *eproc = ps_curproc();
	int r = -EMFILE;

	ke_wait(&eproc->fd_mutex, "dup:eproc->fd_mutex", false, false, -1);

	if (eproc->files[oldfd] == NULL) {
		r = -EBADF;
		goto out;
	}

	for (int i = 0; i < elementsof(eproc->files); i++) {
		if (eproc->files[i] == NULL)
			r = i;
	}

	if (r >= 0) {
		eproc->files[r] = obj_direct_retain(eproc->files[oldfd]);
	}

out:
	ke_mutex_release(&eproc->fd_mutex);
	return r;
}

int
sys_dup3(int oldfd, int newfd, int flags)
{
	eprocess_t *eproc = ps_curproc();
	int r = newfd;

	ke_wait(&eproc->fd_mutex, "dup3:eproc->fd_mutex", false, false, -1);

	if (newfd > 63) {
		r = -EBADF;
		goto out;
	}

	if (eproc->files[oldfd] == NULL) {
		r = -EBADF;
		goto out;
	}

	if (eproc->files[newfd] != NULL) {
		do_close(eproc, newfd);
	}

	eproc->files[newfd] = obj_direct_retain(eproc->files[oldfd]);

out:
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
sys_readlink(const char *path, char *buf, size_t bufsize)
{
#if 0
	int r;
	char *mypath = strdup(path);
	vnode_t *vn;


	//r = vfs_lookup(root_vnode, &vn, mypath, kLookupNoFollow, NULL);

	return r;
#else
	kfatal("readlink(%s): unimplemented\n", path);
#endif
}

uintptr_t
sys_readdir(int fd, void *buf, size_t bufsize)
{
	struct file *file = ps_getfile(ps_curproc(), fd);
	size_t bytes_read = 0;
	int r;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_READDIR(fd: %d, bufsize: %lu off: %lu)\n", fd, bufsize,
	    file->offset);
#endif

	if (file == NULL)
		return -EBADF;
	else if (file->vn->type != VDIR)
		return -ENOTDIR;
	else if (bufsize < DIRENT_RECLEN(NAME_MAX))
		return -EINVAL;

	r = VOP_READDIR(file->vn, buf, bufsize, &bytes_read, file->offset);
	if (r < 0) {
		return r;
	}

	file->offset = r;

	return bytes_read;
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
		for (;;)
			;
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
	kdprintf("SYS_SEEK(offset: %ld)\n", offset);
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

#if DEBUG_SYSCALLS == 1
	char *mypath = strdup(path);
	kdprintf("SYS_STAT(kind: %d, fd: %d, path: %s, flags: %d)\n", kind, fd,
	    mypath, flags);
	kmem_strfree(mypath);
#endif

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

	case VFIFO:
		sb->st_mode |= S_IFIFO;
		break;

	case VNON:
		kfatal("Should be unreachable!\n");
	}

	sb->st_size = vn->size;
	sb->st_blocks = vn->size / 512;
	sb->st_blksize = 512;
	sb->st_atim = vattr.atim;
	sb->st_ctim = vattr.ctim;
	sb->st_mtim = vattr.mtim;
	sb->st_ino = vattr.ino;

out:
	if (pathcpy != NULL)
		kmem_strfree(pathcpy);

	return r;
}

int
sys_unlinkat(int fd, const char *path, int flags)
{
	int r;
	vnode_t *vn = NULL, *dvn_with_file = NULL;
	char *pathcpy;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_UNLINKAT(fd: %d, name: %s, flags: %d)", fd, path, flags);
#endif

	if (fd == AT_FDCWD) {
		vn = root_vnode;
	} else {
		struct file *file = ps_getfile(ps_curproc(), fd);

		if (file == NULL) {
			r = -EBADF;
			goto out;
		}

		vn = file->vn;
	}

	pathcpy = strdup(path);

	r = vfs_lookup(vn, &dvn_with_file, pathcpy, kLookup2ndLast, NULL);
	if (r != 0)
		goto out;

	if (flags & AT_REMOVEDIR) {
		kdprintf("warning: AT_REMOVEDIR not implemented yet\n");
		r = -ENOTDIR;
		goto out;
	} else {
		char *lastname = pathcpy + strlen(pathcpy);
		while (*(lastname - 1) != '/' && (lastname != pathcpy))
			lastname--;
		kassert(dvn_with_file != NULL);
		kassert(dvn_with_file->ops->remove != NULL);
		r = VOP_REMOVE(dvn_with_file, lastname);
	}

out:
	if (dvn_with_file)
		obj_direct_release(dvn_with_file);
	if (vn)
		obj_direct_release(vn);

	return r;
}

uintptr_t
sys_ppoll(struct pollfd *pfds, int nfds, const struct timespec *timeout,
    const sigset_t *sigmask)
{
	int r;
	struct epoll *epoll;

	kassert(nfds >= 0 && nfds < 25);

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
	if (nfds > 0)
		revents = kmem_alloc(sizeof(struct epoll_event) * nfds);

	nanosecs_t nanosecs;
	if (!timeout)
		nanosecs = -1;
	else if (timeout->tv_nsec <= 1000 && timeout->tv_nsec == 0)
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

	case kPXSysMunmap:
		RET = 0;
		break;

	/* file syscalls */
	case kPXSysIOCtl:
		RET = sys_ioctl(ARG1, ARG2, (void *)ARG3);
		break;

	case kPXSysOpenAt:
		RET = sys_openat(ARG1, (const char *)ARG2, ARG3, ARG4);
		break;

	case kPXSysClose:
		RET = sys_close(ARG1);
		break;

	case kPXSysRead:
		RET = sys_read(ARG1, (void *)ARG2, ARG3);
		break;

	case kPXSysReadLink:
		RET = sys_readlink((const char *)ARG1, (char *)ARG2, ARG3);
		break;

	case kPXSysReadDir:
		RET = sys_readdir(ARG1, (void *)ARG2, ARG3);
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

	case kPXSysUnlinkAt:
		RET = sys_unlinkat(ARG1, (const char *)ARG2, ARG3);
		break;

	case kPXSysPPoll:
		RET = sys_ppoll((struct pollfd *)ARG1, ARG2,
		    (const struct timespec *)ARG3, (const sigset_t *)ARG4);
		break;

	case kPXSysIsATTY:
		RET = 0;
		break;

	case kPXSysGetCWD:
		strcpy((char *)ARG1, "/");
		break;

	case kPXSysPipe:
		RET = sys_pipe((int *)ARG1, ARG2);
		break;

	case kPXSysDup:
		RET = sys_dup(ARG1);
		break;

	case kPXSysDup3:
		RET = sys_dup3(ARG1, ARG2, ARG2);
		break;

	/* process & misc misc */
	case kPXSysFork:
		RET = sys_fork(frame);
		break;

	case kPXSysExecVE:
		RET = sys_exec(px_curproc(), (char *)ARG1, (const char **)ARG2,
		    (const char **)ARG3, frame);
		break;

	case kPXSysExit:
		psx_exit(ARG1);
		__builtin_unreachable();

	case kPXSysWaitPID:
		RET = psx_waitpid(ARG1, (int *)ARG2, ARG3);
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

	case kPXSysSetPGID:
		RET = psx_setpgid(ARG1, ARG2);
		break;

	case kPXSysGetPGID:
		RET = psx_getpgid(ARG1);
		break;

	case kPXSysSigMask:
		RET = psx_sigmask(ARG1, (const sigset_t *)ARG2,
		    (sigset_t *)ARG3);
		break;

	case kPXSysSigAction:
		RET = psx_sigaction(ARG1, (const struct sigaction *)ARG2,
		    (struct sigaction *)ARG3);
		break;

	case kPXSysUTSName: {
		struct utsname *buf = (void *)ARG1;
		strcpy(buf->sysname, "Keyronex");
		strcpy(buf->nodename, "keyronex");
		strcpy(buf->release, "v0.7");
		strcpy(buf->version, __DATE__ " " __TIME__);
		strcpy(buf->machine, "amd64");
		break;
	}

	case kPXSysSigEntry:
		px_curproc()->sigentry = ARG1;
		break;

	case kPXSysSigReturn:
		kfatal("Sigreturn\n");
		break;

	default:
		kfatal("Unknown syscall %lu\n", frame->rax);
	}

	return true;
}