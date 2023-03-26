/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 23 2023.
 */

#include "abi-bits/errno.h"
#include "abi-bits/fcntl.h"
#include "abi-bits/seek-whence.h"
#include "abi-bits/vm-flags.h"
#include "amd64.h"
#include "kdk/amd64/portio.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
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

#if DEBUG_SYSCALLS == 0
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

#if 0
	kdprintf("SYS_READ(%d, nbytes: %lu off: %lu)\n", fd, nbyte, file->pos);
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

#if 0
	kdprintf("SYS_WRITE(%d, nbytes: %lu off: %lu)\n", fd, nbyte, file->pos);
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

	/* process & misc misc */
	case kPXSysExecVE:
		RET = sys_exec(px_curproc(), (char *)ARG1, (const char **)ARG2,
		    (const char **)ARG3, frame);
		break;

	case kPXSysSetFSBase:
		ke_curthread()->hl.fs = ARG1;
		wrmsr(kAMD64MSRFSBase, ARG1);
		RET = 0;
		break;

	default:
		kfatal("Unknown syscall %lu\n", frame->rax);
	}

	return true;
}