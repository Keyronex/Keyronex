#include <sys/mman.h>
#include <vfs/vfs.h>
#include <errno.h>
#include <posix/proc.h>

int
vm_mmap(proc_t *proc, void **addr, size_t len, int prot, int flags, int fd,
    off_t offset)
{
	if (flags & MAP_FIXED && PGROUNDDOWN(*addr) != (uintptr_t)*addr)
		return -EINVAL; /* must be page-aligned */
	else if (PGROUNDDOWN(offset) != offset)
		return -EINVAL;

#if DEBUG_SYSCALLS == 1
	kprintf("VM_POSIX: mmap addr %p, len %lu, prot %d, flags %d, fd %d, "
		"offs %ld\n",
	    *addr, len, prot, flags, fd, offset);
#endif

	if (!(flags & MAP_ANON)) {
		file_t *file;

		if (fd == -1 || fd > 64)
			return -EBADF;

		file = proc->files[fd];
		if (!file)
			return -EBADF;

		/* TODO(low): introduce a vnode mmap operation (for devices) */

		return vm_map_object(proc->kproc->map, file->vn->vmobj, (vaddr_t*)addr,
		    len, offset, flags & MAP_PRIVATE);
	} else {
		return vm_allocate(proc->kproc->map, NULL, (vaddr_t*)addr, len);
	}

	return -ENOTSUP;
}
