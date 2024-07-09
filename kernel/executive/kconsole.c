/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Tue Jul 09 2024.
 */
/*!
 * @file kconsole.c
 * @brief Kernel console.
 */

#include <abi-bits/errno.h>

#include "kdk/executive.h"
#include "kdk/file.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/vfs.h"

static vnode_t *kconsole_vnode;
static struct vnode_ops console_vnops;

static io_result_t
io_result(int result, size_t count)
{
	return (io_result_t) { .result = result, .count = count };
}

int
copyin(vaddr_t udata, size_t len, char **out)
{
	char *kdata;
	kdata = kmem_alloc(len);
	if (kdata == NULL)
		return -ENOMEM;
	memcpy(kdata, (void *)udata, len);
	*out = kdata;
	return 0;
}

void
ex_console_init(void)
{
	kconsole_vnode = vnode_new(NULL, VCHR, &console_vnops, NULL, NULL, 0);
}

ex_desc_ret_t
ex_console_open(eprocess_t *proc)
{
	descnum_t num;
	file_t *file;

	file = ex_file_new();
	file->vnode = kconsole_vnode;
	file->nch = (namecache_handle_t) { NULL, NULL };

	num = ex_object_space_reserve(proc->objspace, false);
	if (num == DESCNUM_NULL) {
		ex_file_free((obj_t *)file);
		return -EMFILE;
	}

	ex_object_space_reserved_insert(proc->objspace, num, file);

	return num;
}

io_result_t
console_write(vnode_t *vnode, vaddr_t user_addr, io_off_t off, size_t size)
{
	char *buf;
	int r;

	(void)vnode;
	(void)off;

	r = copyin(user_addr, size, &buf);
	if (r != 0) {
		kmem_free(buf, size);
		return io_result(-r, 0);
	}

	for (int i = 0; i < size; i++)
		kputc(buf[i], NULL);

	kmem_free(buf, size);

	return io_result(0, size);
}

static int
console_seek(vnode_t *vnode, io_off_t old_offset, io_off_t *new_offset)
{
	return -ESPIPE;
}

static struct vnode_ops console_vnops = {
	.cached_write = console_write,
	.seek = console_seek
};
