/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Mon Mar 06 2023.
 */

#include "abi-bits/errno.h"
#include "kdk/kernel.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"

static void
vn_lock_for_paging(vnode_t *vn)
{
	kwaitstatus_t w;
	w = ke_wait(&vn->lock, "pgcache_read:vn->lock", false, false, -1);
	kassert(w == kKernWaitStatusOK);
	vn->locked_for_paging = true;
}

static void
vn_unlock_for_paging(vnode_t *vn)
{
	vn->locked_for_paging = false;
	ke_mutex_release(&vn->lock);
}

int
pgcache_read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	vaddr_t vaddr = -1;
	int r;

	if (vn->type != VREG)
		return -EINVAL;

	if (nbyte == 0)
		return 0;

	vn_lock_for_paging(vn);

	if (off + nbyte > vn->size)
		nbyte = vn->size <= off ? 0 : vn->size - off;

	if (nbyte == 0)
		goto out;

	r = vm_map_object(kernel_process.map, &vn->vmobj, &vaddr,
	    PGROUNDUP(nbyte + off), 0x0, kVMRead, kVMRead, kVMInheritShared,
	    false, false);
	kassert(r == 0);

	memcpy(buf, (void *)(vaddr + off), nbyte);

	r = vm_map_deallocate(kernel_process.map, vaddr,
	    PGROUNDUP(nbyte + off));
	kassert(r == 0);

out:
	vn_unlock_for_paging(vn);

	return nbyte;
}

int
pgcache_write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	vaddr_t vaddr = -1;
	int r;

	if (vn->type != VREG)
		return -EINVAL;

	if (nbyte == 0)
		return 0;

	vn_lock_for_paging(vn);

	if (off + nbyte > vn->size)
		vn->size = off + nbyte;

	r = vm_map_object(kernel_process.map, &vn->vmobj, &vaddr,
	    PGROUNDUP(nbyte + off), 0x0, kVMAll, kVMAll, kVMInheritShared,
	    false, false);
	kassert(r == 0);

	memcpy((void *)(vaddr + off), buf, nbyte);

	r = vm_map_deallocate(kernel_process.map, vaddr,
	    PGROUNDUP(nbyte + off));

	vn_unlock_for_paging(vn);

	return nbyte;
}