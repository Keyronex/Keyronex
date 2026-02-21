/*
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 * Created on Sun Dec 14 2025.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file mmap.c
 * @brief mmap syscalls.
 */

#include <sys/errno.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/vm.h>
#include <sys/vnode.h>

void *
sys_mmap(void *addr, size_t len, int prot, int flags, int fildes, off_t offset)
{
	vm_prot_t vmprot = 0;
	vnode_t *vn = NULL;
	vaddr_t hint = (vaddr_t)addr;
	int r;

	if (flags & MAP_PRIVATE && flags & MAP_SHARED)
		return (void *)-EINVAL;

	if (len == 0 || hint % PGSIZE != 0 || offset % PGSIZE != 0)
		return (void *)-EINVAL;

	len = roundup2(len, PGSIZE);

	if (prot & PROT_READ)
		vmprot |= VM_READ;
	if (prot & PROT_WRITE)
		vmprot |= VM_WRITE;
	if (prot & PROT_EXEC)
		vmprot |= VM_EXEC;

	if (flags & MAP_ANONYMOUS) {
		if (fildes != -1)
			return (void *)-EINVAL;
	} else {
		file_t *file = uf_lookup(curproc()->finfo, fildes);
		if (file == NULL)
			return (void *)-EBADF;
		vn = file->vnode;
		file_release(file);
	}

	if (flags & MAP_SHARED && flags & MAP_ANONYMOUS) {
		kfatal("shared anonymous mmap not implemented yet\n");
	} else if (flags & MAP_ANONYMOUS) {
		r = vm_allocate(thread_vm_map(curthread()), vmprot, &hint, len,
		    flags & MAP_FIXED);
	} else if (flags & MAP_SHARED) {
		if (vn->ops->mmap != NULL) {
			r = vn->ops->mmap(addr, len, prot, flags, vn, offset,
			    &hint);
		} else {
			kassert(vn->type == VREG);
			r = vm_map(thread_vm_map(curthread()), vn->file.vmobj,
			    &hint, len, offset, vmprot, vmprot, true, false,
			    flags & MAP_FIXED);
		}
	} else {
		if (vn->ops->mmap != NULL)
			return (void *)-ENOSYS; /* only shared mmap for devs */

		r = vm_map(thread_vm_map(curthread()), vn->file.vmobj, &hint,
		    len, offset, vmprot, vmprot, false, true,
		    flags & MAP_FIXED);
	}

	if (r == 0)
		return (void *)hint;
	else
		return (void *)(uintptr_t)r;
}
