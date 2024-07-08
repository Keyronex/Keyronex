/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Jul 08 2024.
 */
/*!
 * @file file.c
 * @brief File object type and services.
 */

#include <sys/errno.h>

#include "kdk/executive.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "object.h"
#include "kdk/file.h"

/* objman.c */
extern obj_class_t file_class;

size_t
user_strlen(const char *user_str)
{
	return strlen(user_str);
}

int
copyout_str(const char *ustr, char **out)
{
	int len;
	char *kstr;

	len = user_strlen(ustr);
	kstr = kmem_alloc(len + 1);
	memcpy(kstr, ustr, len);

	kstr[len] = '\0';
	*out = kstr;
	return 0;
}

ex_desc_ret_t
ex_service_file_open(eprocess_t *proc, const char *upath)
{
	char *path;
	namecache_handle_t nch;
	descnum_t descnum;
	int r;

	r = copyout_str(upath, &path);
	if (r != 0)
		return r;

	r = vfs_lookup(root_nch, &nch, path, 0);
#if TRACE_SYSCALLS
	if (r != 0)
		kprintf("Couldn't find <%s>\n", upath);
#endif

	if (r == 0) {
		struct file *file;

		r = obj_new(&file, file_class, sizeof(struct file), NULL);
		kassert(r == 0);
		file->nch = nch;
		file->offset = 0;

		descnum = ex_object_space_reserve(proc->objspace, false);
		if (descnum == DESCNUM_NULL)
			return -EMFILE;

		ex_object_space_reserved_insert(proc->objspace, descnum, file);

		return descnum;
	} else
		return r;
}

ex_size_ret_t
ex_service_file_read_cached(eprocess_t *proc, descnum_t handle, vaddr_t ubuf,
    size_t count)
{
	void *obj;
	struct file *file;
	int r;

	obj = ex_object_space_lookup(proc->objspace, handle);
	if (obj == NULL)
		return -EBADF;

	file = obj;

	r = ubc_io(file->nch.nc->vp, ubuf, file->offset, count, false);
	kassert(r >= 0);
	file->offset += r;
	obj_release(file);

	return r;
}

ex_size_ret_t
ex_service_file_write_cached(eprocess_t *proc, descnum_t handle, vaddr_t ubuf, size_t count)
{
	kfatal("Implement me!\n");
}

ex_off_ret_t
ex_service_file_seek(eprocess_t *proc, descnum_t handle, io_off_t offset)
{
	void *obj;
	struct file *file;

	obj = ex_object_space_lookup(proc->objspace, handle);
	if (obj == NULL)
		return -EBADF;

	file = obj;
	file->offset = offset;
	obj_release(file);

	return file->offset;
}
