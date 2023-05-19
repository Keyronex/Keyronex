/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#include <fcntl.h>
#include <keyronex/syscall.h>

#include "amd64.h"
#include "executive/epoll.h"
#include "kdk/amd64/mdamd64.h"
#include "kdk/amd64/portio.h"
#include "kdk/kerndefs.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"
#include "keysock/sockfs.h"
#include "posix/pxp.h"

/* devmgr/fifofs.c */
int sys_pipe(int *out, int flags);
/* executive/?.c */
int sys_dup3(int oldfd, int newfd, int flags);

#if 0
#define DEBUG_SYSCALLS 1
#define DEBUG_FD_SYSCALLS 1
#endif

static void
setup_file(eprocess_t *eproc, int fd, vnode_t *vn, int flags)
{
	eproc->files[fd] = kmem_alloc(sizeof(struct file));
	obj_initialise_header(&eproc->files[fd]->objhdr, kObjTypeFile);
	eproc->files[fd]->offset = 0;
	eproc->files[fd]->flags = flags;
	eproc->files[fd]->vn = vn;
}

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

		if (file->vn->ops->mmap != NULL) {
			r = file->vn->ops->mmap(file->vn, proc->map, &at_addr,
			    len, offset, vmprot, kVMAll,
			    private ? kVMInheritCopy : kVMInheritShared,
			    flags & MAP_FIXED, private);
		} else {
			kassert(file->vn->vmobj != NULL);
			r = vm_map_object(proc->map, file->vn->vmobj, &at_addr,
			    len, offset, vmprot, kVMAll,
			    private ? kVMInheritCopy : kVMInheritShared,
			    flags & MAP_FIXED, private);
		}
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

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_IOCTL(fd: %d, command: 0x%lx\n", fd, command);
#endif

	if (file == NULL)
		return -EBADF;

	if (command == FIOCLEX) {
		/* TODO: CLOEXEC */
		return 0;
	}

	if (file->vn->ops->ioctl == NULL) {
		kdprintf("no ioctl on this file\n");
		return -ENOSYS;
	}

	return VOP_IOCTL(file->vn, command, data);
}

int
posix_do_openat(vnode_t *dvn, const char *path, int flags, int mode)
{
	eprocess_t *eproc = ps_curproc();
	vnode_t *vn = NULL;
	int r;
	int fd;

	r = ps_allocfiles(1, &fd);

#if DEBUG_SYSCALLS == 1
	kdprintf("%d: openat(%d,%s) to FD %d\n", eproc->id, mode, path, fd);
#endif

	if (fd == -1)
		return -ENFILE;

	r = vfs_lookup(dvn, &vn, path, 0);
	if (r < 0 && flags & O_CREAT) {
		vattr_t attr;
		vnode_t *final_dirvn;
		const char *lastpart;

		attr.mode = S_IFREG | mode;
		attr.type = VREG;

		r = vfs_lookup_for_at(dvn, &final_dirvn, path, &lastpart);
		if (r != 0)
			goto out;

		r = VOP_CREAT(final_dirvn, &vn, lastpart, &attr);
		obj_direct_release(&final_dirvn);
		if (r != 0)
			goto out;
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

	setup_file(ps_curproc(), fd, vn, flags);

	r = fd;

#if DEBUG_FD_SYSCALLS == 1
	kdprintf("(%d) Opened new FD %d\n", ps_curproc()->id, r);
#endif

out:
	if (r < 0) {
		if (fd != -1)
			eproc->files[fd] = NULL;
		if (vn != NULL)
			obj_direct_release(vn);
	}

	return r;
}

int
sys_openat(int dirfd, const char *path, int flags, mode_t mode)
{
	vnode_t *dvn;
	mode_t umask = __atomic_load_n(&px_curproc()->umask, __ATOMIC_SEQ_CST);

	/* note: will need refcount dealt with */
	if (dirfd == AT_FDCWD) {
		dvn = ps_curcwd();
	} else {
		kfatal("Unimplemented\n");
	}

	return posix_do_openat(dvn, path, flags, mode & ~umask);
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

#if DEBUG_FD_SYSCALLS == 1
	if (r == 0) {
		kdprintf("%d: Closed FD %d\n", eproc->id, fd);
	} else {
		kdprintf("%d: Failed to close FD %d\n", eproc->id, fd);
	}
#endif

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
		if (eproc->files[i] == NULL) {
			r = i;
			break;
		}
	}

	if (r >= 0)
		eproc->files[r] = obj_direct_retain(eproc->files[oldfd]);

#if DEBUG_FD_SYSCALLS == 1
	kdprintf("%d: dup %d into %d\n", eproc->id, oldfd, r);
#endif

out:
	ke_mutex_release(&eproc->fd_mutex);
	return r;
}

int
sys_dup3(int oldfd, int newfd, int flags)
{
	eprocess_t *eproc = ps_curproc();
	int r = newfd;

	/* TODO: handle this properly, wget does it */
	if (oldfd == newfd)
		return newfd;

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
		eproc->files[newfd] = NULL;
	}

	if (eproc->files[oldfd] != NULL)
		eproc->files[newfd] = obj_direct_retain(eproc->files[oldfd]);

#if DEBUG_FD_SYSCALLS == 1
	kdprintf("%d: dup3 %d into %d\n", eproc->id, oldfd, newfd);
#endif

out:
	ke_mutex_release(&eproc->fd_mutex);
	return r;
}

char *
lsb_basename(const char *filename)
{
	char *p = strrchr(filename, '/');
	return p ? p + 1 : (char *)filename;
}

int
sys_link(const char *oldpath, const char *newpath)
{
	int r;
	vnode_t *oldvn = NULL, *dvn_for_new = NULL;
	char *pathcpy, *newpathcpy;
	const char *newname;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_LINK(old: %s, new: %s)\n", oldpath, newpath);
#endif

	pathcpy = strdup(oldpath);
	newpathcpy = strdup(newpath);
	newname = lsb_basename(newpathcpy);

	r = vfs_lookup(oldvn, &oldvn, pathcpy, 0);
	if (r != 0)
		goto out;

	r = vfs_lookup(oldvn, &dvn_for_new, newpathcpy, kLookup2ndLast);
	if (r != 0)
		goto out;

	r = VOP_LINK(dvn_for_new, oldvn, newname);
	if (r != 0)
		goto out;

out:
	kmem_strfree(pathcpy);
	kmem_strfree(newpathcpy);

	if (dvn_for_new)
		obj_direct_release(dvn_for_new);
	if (oldvn)
		obj_direct_release(oldvn);

	return r;
}

int
sys_chdir(const char *path)
{
	eprocess_t *eproc = ps_curproc();
	int r;
	vnode_t *vn = NULL, *cwd;
	kwaitstatus_t w;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_CHDIR(%s)\n", path);
#endif

	cwd = ps_curcwd();
	r = vfs_lookup(cwd, &vn, path, 0);
	if (r != 0)
		return r;

	if (vn->type != VDIR) {
		obj_direct_release(vn);
		return -ENOTDIR;
	}

	if (cwd)
		obj_direct_release(cwd);

	w = ke_wait(&eproc->fd_mutex, "sys_chdir:eproc->fd_mutex", false, false,
	    -1);
	kassert(w == kKernWaitStatusOK);
	eproc->cwd = vn;
	ke_mutex_release(&eproc->fd_mutex);

	return 0;
}

mode_t
sys_umask(mode_t mode)
{
	mode_t umask = __atomic_load_n(&px_curproc()->umask, __ATOMIC_ACQUIRE);

	mode &= 0777;

#if DEBUG_SYSCALLS == 1
	kdprintf("SYS_UMASK(%o)\n", mode);
#endif

	__atomic_store_n(&px_curproc()->umask, mode, __ATOMIC_RELEASE);
	return umask;
}

int
sys_mkdirat(int dirfd, const char *path, mode_t mode)
{
	int r;
	vnode_t *vn = NULL, *new_vn = NULL;
	char *pathcpy, *lastname;
	vattr_t attr;

	pathcpy = strdup(path);

#if DEBUG_SYSCALLS == 0
	kdprintf("SYS_MKDIRAT(fd: %d, name: %s, flags: %d)\n", dirfd, pathcpy,
	    mode);
#endif

	if (dirfd == AT_FDCWD) {
		vn = ps_curcwd();
	} else {
		struct file *file = ps_getfile(ps_curproc(), dirfd);

		if (file == NULL) {
			r = -EBADF;
			goto out;
		}

		vn = file->vn;
	}

	r = vfs_lookup(vn, &vn, pathcpy, kLookup2ndLast);
	if (r != 0)
		goto out;

	lastname = pathcpy + strlen(pathcpy);

	/* drop trailing slash; do we need this? */
	if (*(lastname - 1) == '/') {
		lastname -= 1;
		if (lastname == pathcpy) {
			r = -EINVAL;
			goto out;
		}
	}

	while (*(lastname - 1) != '/' && (lastname != pathcpy))
		lastname--;

	attr.mode = (mode & 01777) & ~PSX_GETUMASK();
	r = VOP_MKDIR(vn, &new_vn, lastname, &attr);

out:
	if (new_vn)
		obj_direct_release(new_vn);
	if (vn)
		obj_direct_release(vn);
	if (pathcpy)
		kmem_strfree(pathcpy);

	return r;
}

static int
get_fd_vn(int fd, vnode_t **out)
{
	if (fd == AT_FDCWD) {
		*out = ps_curcwd();
	} else {
		struct file *file = ps_getfile(ps_curproc(), fd);

		if (file == NULL)
			return -EBADF;

		*out = file->vn;
	}

	return 0;
}

int
sys_renameat(int orig_dirfd, const char *orig_path, int new_dirfd,
    const char *new_path)
{
	int r;
	vnode_t *orig_dvn = NULL, *new_dvn = NULL;
	char *orig_pathcpy, *orig_pathlast;
	char *new_pathcpy, *new_pathlast;

	orig_pathcpy = strdup(orig_path);
	new_pathcpy = strdup(new_path);

#if DEBUG_SYSCALLS == 0
	kdprintf(
	    "SYS_RENAMEAT(orig_fd: %d, orig_path: %s, new_fd: %d, new_path: %s)\n",
	    orig_dirfd, orig_pathcpy, new_dirfd, new_pathcpy);
#endif

	r = get_fd_vn(orig_dirfd, &orig_dvn);
	if (r != 0)
		goto out;

	r = get_fd_vn(new_dirfd, &new_dvn);
	if (r != 0)
		goto out;

	/* todo: these are incompatible with refcounting */
	r = vfs_lookup(orig_dvn, &orig_dvn, orig_pathcpy, kLookup2ndLast);
	if (r != 0)
		goto out;

	r = vfs_lookup(new_dvn, &new_dvn, new_pathcpy, kLookup2ndLast);
	if (r != 0)
		goto out;

	/* todo: break out into a function, factor with other *at code */
	orig_pathlast = orig_pathcpy + strlen(orig_pathcpy);
	while (*(orig_pathlast - 1) != '/' && (orig_pathlast != orig_pathcpy))
		orig_pathlast--;

	new_pathlast = new_pathcpy + strlen(new_pathcpy);
	while (*(new_pathlast - 1) != '/' && (new_pathlast != new_pathcpy))
		new_pathlast--;

	kassert(orig_dvn->ops->rename != NULL);
	r = VOP_RENAME(orig_dvn, orig_pathlast, new_dvn, new_pathlast);

out:
	if (new_dvn)
		obj_direct_release(new_dvn);
	if (orig_dvn)
		obj_direct_release(orig_dvn);
	if (new_pathcpy)
		kmem_strfree(new_pathcpy);
	if (orig_pathcpy)
		kmem_strfree(orig_pathcpy);

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

	r = VOP_READ(file->vn, buf, nbyte, file->offset, file->flags);
	if (r < 0) {
#if DEBUG_SYSCALL_ERRORS == 1
		kdprintf("VOP_READ got %d\n", r);
#endif
		return r;
	}

	file->offset += r;

	return r;
}

int
sys_readlink(const char *path, char *buf, size_t bufsize)
{
	int r;
	char *pathcpy = strdup(path), *link = NULL;
	size_t linklen;
	vnode_t *vn = NULL;

	r = vfs_lookup(ps_curcwd(), &vn, pathcpy, kLookupNoFollowFinalSymlink);
	if (r != 0)
		goto out;

	link = kmem_alloc(256);
	r = VOP_READLINK(vn, link);
	vn = obj_direct_release(vn);

	if (r != 0)
		goto out;

	linklen = strlen(link);
	if (linklen > bufsize + 1) {
		r = -ENAMETOOLONG;
	} else {
		r = linklen + 1;
		memcpy(buf, link, linklen + 1);
	}

out:
	kmem_strfree(pathcpy);
	if (link != NULL)
		kmem_free(link, 256);

	return r;
}

uintptr_t
sys_readdir(int fd, void *buf, size_t bufsize)
{
	struct file *file = ps_getfile(ps_curproc(), fd);
	size_t bytes_read = 0;
	off_t r;

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

	r = VOP_WRITE(file->vn, buf, nbyte, file->offset, file->flags);
	if (r < 0) {
		kdprintf("VOP_WRITE got %d\n", r);
		for (;;)
			;
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
	vnode_t *vn = NULL;
	vattr_t vattr;
	char *pathcpy = NULL;
	int lookupflags = 0;

	if (flags & AT_SYMLINK_NOFOLLOW)
		lookupflags |= kLookupNoFollowFinalSymlink;

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

		r = vfs_lookup(file->vn, &vn, pathcpy, lookupflags);
		if (r != 0)
			goto out;
	} else if (kind == kPXStatKindCWD) {
		pathcpy = strdup(path);

		r = vfs_lookup(ps_curcwd(), &vn, pathcpy, lookupflags);
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
	if (r != 0)
		goto out;

	memset(sb, 0x0, sizeof(*sb));
	sb->st_mode = vattr.mode & ~S_IFMT;

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
	sb->st_dev = (dev_t)vn->vfsp;

out:
	if (vn != NULL)
		obj_direct_release(vn);

	if (pathcpy != NULL)
		kmem_strfree(pathcpy);

	return r;
}

int
sys_statfs(const char *path, struct statfs *out)
{
	vnode_t *vn;
	int r;

	r = vfs_lookup(root_vnode, &vn, path, 0);
	if (r != 0)
		return r;

	r = vn->vfsp->ops->statfs(vn->vfsp, out);
	obj_direct_release(vn);

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
		vn = obj_direct_retain(ps_curcwd());
	} else {
		struct file *file = ps_getfile(ps_curproc(), fd);

		if (file == NULL) {
			r = -EBADF;
			goto out;
		}

		vn = obj_direct_retain(file->vn);
	}

	pathcpy = strdup(path);

	r = vfs_lookup(vn, &dvn_with_file, pathcpy, kLookup2ndLast);
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

static int
sys_fcntl(int fd, int request, uint64_t arg)
{
	struct file *file = ps_getfile(ps_curproc(), fd);
	int r = 0;

	if (file == NULL)
		return -EBADF;

	switch (request) {
#if 0
	case F_GETFD:
		if (file->flags & O_CLOEXEC)
			r = O_CLOEXEC;
		break;

	case F_SETFD:
		if (arg & O_CLOEXEC)
			file->flags |= O_CLOEXEC;
		else
			file->flags &= ~O_CLOEXEC;
		break;
#endif

	case F_SETFL:
		file->flags = arg;
		break;

	case F_GETFL:
		r = file->flags;
		break;

	default:
		kdprintf("Unhandled fcntl request %d\n", request);
	}

	return r;
}

uintptr_t
sys_ppoll(struct pollfd *pfds, int nfds, const struct timespec *timeout,
    const sigset_t *sigmask)
{
	int r;
	struct epoll *epoll;

	kassert(nfds >= 0 && nfds < 25);

	epoll = epoll_do_new();

	for (size_t i = 0; i < nfds; i++) {
		struct epoll_event ev;
		struct pollfd *pfd = &pfds[i];

		if (pfd->fd < 0) {
			/*
			 * "If the value of fd is less than 0, events shall be
			 * ignored, and revents shall be set to 0 in that entry
			 * on return from poll()."
			 *
			 * We initialise all revents fields to 0 anyway so press
			 * on.
			 */
			continue;
		}

		ev.data.u32 = i;
		ev.events = 0;
		if (pfd->events & POLLIN)
			ev.events |= EPOLLIN;
		if (pfd->events & POLLOUT)
			ev.events |= EPOLLOUT;

		r = epoll_do_ctl(epoll, EPOLL_CTL_ADD, pfd->fd, &ev);
		switch (r) {
		case 0:
			continue;

		case -EBADF:
			pfds[i].revents = POLLNVAL;
			epoll_do_destroy(epoll);
			return 1;

		default:
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
	else if (timeout->tv_nsec <= 1000 && timeout->tv_sec == 0)
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

int
sys_epoll_create(int flags)
{
	int r;
	int fd;

	r = ps_allocfiles(1, &fd);
	if (r != 0) {
		return r;
	}

	setup_file(ps_curproc(), fd, epoll_new(), flags);

	return fd;
}

int
sys_epoll_ctl(int epfd, int mode, int fd, struct epoll_event *ev)
{
	struct epoll *epoll;

	epoll = epoll_from_vnode(ps_getfile(ps_curproc(), epfd)->vn);
	if (!epoll)
		return -EBADF;

	return epoll_do_ctl(epoll, mode, fd, ev);
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
sys_socket(int domain, int type, int protocol)
{
	vnode_t *vn;
	int fd;
	int r;
	int flags;

	r = ps_allocfiles(1, &fd);
	if (r != 0) {
		return r;
	}

	flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
	type &= ~flags;

	r = sock_create(domain, type, protocol, &vn);
	if (r != 0) {
		ps_curproc()->files[fd] = NULL;
		return r;
	}
	setup_file(ps_curproc(), fd, vn, flags);

#if DEBUG_FD_SYSCALLS == 1
	kdprintf("(%d): Opened new socket %d\n", ps_curproc()->id, fd);
#endif

	return fd;
}

static int
sys_socketpair(int domain, int type, int protocol, int *out)
{
	vnode_t *vn[2] = { 0, 0 };
	int fd[2];
	int r;
	int flags;

	r = ps_allocfiles(2, fd);
	if (r != 0)
		return r;

	flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
	type &= ~flags;

	r = sock_create(domain, type, protocol, &vn[0]);
	if (r != 0)
		goto out;

	r = sock_create(domain, type, protocol, &vn[1]);
	if (r != 0)
		goto out;

	r = sock_pair(vn[0], vn[1]);
	if (r != 0)
		goto out;

	setup_file(ps_curproc(), fd[0], vn[0], flags);
	setup_file(ps_curproc(), fd[0], vn[0], flags);

	memcpy(out, fd, sizeof(fd));

out:
	if (r != 0) {
		if (vn[0] != NULL)
			obj_direct_release(vn[0]);
		if (vn[1] != NULL)
			obj_direct_release(vn[1]);
		ps_curproc()->files[fd[0]] = NULL;
		ps_curproc()->files[fd[1]] = NULL;
	}

	return r;
}

int
sys_accept(int fd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
	vnode_t *vn, *newvn;
	int newfd;
	int r;

	vn = ps_getfile(ps_curproc(), fd)->vn;
	if (!vn)
		return -EBADF;

	r = ps_allocfiles(1, &newfd);
	if (r != 0) {
		return r;
	}

	r = sock_accept(vn, addr, addrlen, &newvn);
	if (r != 0) {
		ps_curproc()->files[newfd] = NULL;
		return r;
	}

	setup_file(ps_curproc(), newfd, newvn, flags);

	return newfd;
}

int
sys_bind(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	vnode_t *vn;

	vn = ps_getfile(ps_curproc(), fd)->vn;
	if (!vn)
		return -EBADF;

	return sock_bind(vn, addr, addrlen);
}

int
sys_connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
	vnode_t *vn;

	if (ps_getfile(ps_curproc(), fd) == NULL)
		return -EBADF;

	vn = ps_getfile(ps_curproc(), fd)->vn;
	if (!vn)
		return -EBADF;

	return sock_connect(vn, addr, addrlen);
}

int
sys_listen(int fd, uint8_t backlog)
{
	vnode_t *vn;

	vn = ps_getfile(ps_curproc(), fd)->vn;
	if (!vn)
		return -EBADF;

	return sock_listen(vn, backlog);
}

int
sys_recvmsg(int fd, struct msghdr *msg, int flags)
{
	struct file *file;

	file = ps_getfile(ps_curproc(), fd);
	if (!file)
		return -EBADF;

	return sock_recvmsg(file->vn, msg,
	    flags | (file->flags & O_NONBLOCK ? MSG_DONTWAIT : 0));
}

int
sys_sendmsg(int fd, struct msghdr *msg, int flags)
{
	vnode_t *vn;

	vn = ps_getfile(ps_curproc(), fd)->vn;
	if (!vn)
		return -EBADF;

	return sock_sendmsg(vn, msg, flags);
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

	case kPXSysStatFS:
		RET = sys_statfs((const char *)ARG1, (struct statfs *)ARG2);
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

	case kPXSysLink:
		RET = sys_link((const char *)ARG1, (const char *)ARG2);
		break;

	case kPXSysChDir:
		RET = sys_chdir((const char *)ARG1);
		break;

	case kPXSysUMask:
		RET = sys_umask((mode_t)ARG1);
		break;

	case kPXSysMkDirAt:
		RET = sys_mkdirat(ARG1, (const char *)ARG2, (mode_t)ARG3);
		break;

	case kPXSysRenameAt:
		RET = sys_renameat(ARG1, (const char *)ARG2, ARG3,
		    (const char *)ARG4);
		break;

	case kPXSysFCntl:
		RET = sys_fcntl(ARG1, ARG2, ARG3);
		break;

	case kPXSysEPollCreate:
		RET = sys_epoll_create(ARG1);
		break;

	case kPXSysEPollCtl:
		RET = sys_epoll_ctl(ARG1, ARG2, ARG3, (void *)ARG4);
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

	case kPXSysGetTID:
		RET = px_curthread()->tid;
		break;

	case kPXSysSigMask:
		RET = psx_sigmask(ARG1, (const sigset_t *)ARG2,
		    (sigset_t *)ARG3);
		break;

	case kPXSysSigSend:
		RET = psx_sigsend(ARG1, ARG2);
		break;

	case kPXSysSigAction:
		RET = psx_sigaction(ARG1, (const struct sigaction *)ARG2,
		    (struct sigaction *)ARG3);
		break;

	case kPXSysSocket:
		RET = sys_socket(ARG1, ARG2, ARG3);
		break;

	case kPXSysSocketPair:
		kfatal("SocketPair\n");
		RET = sys_socketpair(ARG1, ARG2, ARG3, (int *)ARG4);
		break;

	case kPXSysAccept:
		RET = sys_accept(ARG1, (struct sockaddr *)ARG2,
		    (socklen_t *)ARG3, ARG4);
		break;

	case kPXSysBind:
		RET = sys_bind(ARG1, (void *)ARG2, ARG3);
		break;

	case kPXSysConnect:
		RET = sys_connect(ARG1, (void *)ARG2, ARG3);
		break;

	case kPXSysListen:
		RET = sys_listen(ARG1, ARG2);
		break;

	case kPXSysSendMsg:
		RET = sys_sendmsg(ARG1, (struct msghdr *)ARG2, ARG3);
		break;

	case kPXSysRecvMsg:
		RET = sys_recvmsg(ARG1, (struct msghdr *)ARG2, ARG3);
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

	case kPXSysSleep: {
		uint64_t ns = ARG1;
		kevent_t none;
		kwaitstatus_t w;

		kassert(ns > 0);

		ke_event_init(&none, false);

		w = ke_wait(&none, "sys_sleep", true, true, ns);
		switch (w) {
		case kKernWaitStatusTimedOut:
			RET = 0;
			break;

		case kKernWaitStatusSignalled:
			RET = -EINTR;
			break;

		default:
			kfatal("Unexpected sleep return %d\n", w);
		}

		break;
	}

	case kPXSysClockGet: {
		switch (ARG1) {
		case CLOCK_REALTIME:
		case CLOCK_REALTIME_ALARM:
		case CLOCK_REALTIME_COARSE:
			RET = ke_datetime_get();
			break;

		case CLOCK_MONOTONIC:
		case CLOCK_MONOTONIC_COARSE:
		case CLOCK_MONOTONIC_RAW:
			RET = __atomic_load_n(&cpu_bsp.ticks, __ATOMIC_SEQ_CST);
			break;

		default:
			kfatal("Unhandled clock %lu\n", ARG1);
		}
		break;
	}

	case kPXSysForkThread:
		RET = psx_fork_thread(frame, (void *)ARG1, (void *)ARG2);
		break;

	case kPXSysFutexWait:
		RET = sys_futex_wait((int *)ARG1, ARG2,
		    (const struct timespec *)ARG3);
		break;

	case kPXSysFutexWake:
		RET = sys_futex_wake((int *)ARG1);
		break;

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
