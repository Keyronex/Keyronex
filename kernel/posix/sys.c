#include <kern/kmem.h>
#include <libkern/libkern.h>
#include <md/intr.h>
#include <posix/proc.h>
#include <posix/sys.h>

#include <fcntl.h>
#include <errno.h>
#include <amd64/amd64.h>

#define DEBUG_SYSCALLS 1

int vm_mmap(proc_t *proc, void **addr, size_t len, int prot, int flags, int fd,
    off_t offset);

int
sys_open(struct proc *proc, const char *path, int mode)
{
	vnode_t *vn;
	int	 r;
	int	 fd = -1;

	for (int i = 0; i < elementsof(proc->files); i++) {
		if (proc->files[i] == NULL) {
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

	r = vfs_lookup(root_vnode, &vn, path, 0, NULL);
	if (r < 0 && mode & kLookupCreat)
		r = vfs_lookup(root_vnode, &vn, path, kLookupCreat, NULL);

	if (r < 0) {
#if DEBUG_SYSCALLS == 1
		kprintf("lookup returned %d\n", r);
#endif
		return r;
	}

	if (vn->ops->open) {
		vnode_t *oldvn = vn;
		r = VOP_OPEN(oldvn, &vn, mode);
		/* deref oldvn... */
		if (r < 0) {
#if DEBUG_SYSCALLS == 1
			kprintf("open returned %d\n", r);
#endif
			return r;
		}
	}

	proc->files[fd] = kmem_alloc(sizeof(file_t));
	kassert(proc->files[fd] != NULL);
	proc->files[fd]->vn = vn;
	proc->files[fd]->refcnt = 1;
	proc->files[fd]->pos = 0;

	return fd;
}

int
sys_read(struct proc *proc, int fd, void *buf, size_t nbyte)
{
	file_t *file = proc->files[fd];
	int	r;

#if DEBUG_SYSCALLS == 1
	kprintf("SYS_READ(%d, nbytes: %lu off: %lu)\n", fd, nbyte, file->pos);
#endif

	if (file == NULL)
		return -EBADF;

	r = VOP_READ(file->vn, buf, nbyte, file->pos);
	if (r < 0) {
		kprintf("VOP_READ got %d\n", r);
		return r;
	}

	file->pos += r;

	return r;
}

int
sys_seek(struct proc *proc, int fd, off_t offset, int whence)
{
	file_t *file = proc->files[fd];

#if DEBUG_SYSCALLS == 1
	kprintf("SYS_SEEK(offset: %ld)\n", offset);
#endif

	if (file == NULL)
		return -EBADF;

	if (file->vn->type != VREG)
		return -ESPIPE;

	if (whence == SEEK_SET)
		file->pos = offset;
	else if (whence == SEEK_CUR)
		file->pos += offset;
	else if (whence == SEEK_END) {
		vattr_t attr;
		int	r;

		r = file->vn->ops->getattr(file->vn, &attr);
		if (r < 0)
			return -1;

		file->pos = attr.size + offset;
	}

	return file->pos;
}

int
sys_close(struct proc *proc, int fd, uintptr_t *errp)
{
	file_t *file;

	/* lock proc fdlock */
	file = proc->files[fd];

	if (file == NULL) {
		*errp = EBADF;
		return 0;
	}

	//file_unref(file);
	proc->files[fd] = NULL;

	/* todo unlock proc fdlock */

	return 0;
}

int
posix_syscall(md_intr_frame_t *frame)
{
	proc_t *proc = curthread()->process->psxproc;
	// kthread_t *thread = curthread();
	uintptr_t err = 0;

#define ARG1 frame->rdi
#define ARG2 frame->rsi
#define ARG3 frame->rdx
#define ARG4 frame->r10
#define ARG5 frame->r8
#define ARG6 frame->r9

#define RET frame->rax
#define ERR frame->rdi

	switch (frame->rax) {
	case kPXSysDebug: {
		nk_dbg("SYS_POSIX: %s\n", (char *)ARG1);
		md_intr_frame_trace(frame);
		break;
	}

	case kPXSysExecVE: {
		int r = sys_exec(proc, (char *)ARG1, (const char **)ARG2,
		    (const char **)ARG3, frame);

		if (r < 0) {
			RET = -1;
			err = -r;
		}

		break;
	}

	case kPXSysMmap: {
		void *addr = (void *)ARG1 == NULL ? (void *)VADDR_MAX :
						    (void *)ARG1;
		err = -vm_mmap(proc, &addr, ARG2, ARG3, ARG4, ARG5, ARG6);
		RET = (uintptr_t)addr;
		break;
	}

	case kPXSysOpen: {
		int r = sys_open(proc, (const char *)ARG1, ARG2);
		if (r < 0) {
			RET = -1;
			err = -r;
		} else {
			RET = r;
			err = 0;
		}
		break;
	}

	case kPXSysRead: {
		int r = sys_read(proc, ARG1, (void *)ARG2, ARG3);
		if (r < 0) {
			RET = -1;
			err = -r;
		} else {
			RET = r;
			err = 0;
		}
		break;
	}

	case kPXSysSeek: {
		int r = sys_seek(proc, ARG1, ARG2, ARG3);
		if (r < 0) {
			RET = -1;
			err = -r;
		} else {
			RET = r;
			err = 0;
		}
		break;
	}

	case kPXSysClose: {
		RET = sys_close(proc, ARG1, &err);
		break;
	}

	case kPXSysSetFSBase: {
		curthread()->fs = ARG1;
		wrmsr(kAMD64MSRFSBase, ARG1);
		RET = 0;
		break;
	}

	default: {
		nk_fatal("unhandled syscall number %lu\n", frame->rax);
	}
	}

	ERR = err;
	return 0;
}
