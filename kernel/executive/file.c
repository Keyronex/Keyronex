/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Mon Jul 08 2024.
 */
/*!
 * @file file.c
 * @brief File object type and services.
 */

#include <sys/errno.h>

#include <abi-bits/seek-whence.h>

#include "kdk/executive.h"
#include "kdk/file.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"
#include "object.h"

/* objman.c */
extern obj_class_t file_class;

size_t
user_strlen(const char *user_str)
{
	return strlen(user_str);
}

int
copyin_str(const char *ustr, char **out)
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

void
ex_file_free(obj_t *obj)
{
#if 0
	file_t *file = file;
	if (file->nch.nc != NULL)
		nchandle_release(file->nch);
	else
	 	vn_release(file->vnode);
#else
	(void)obj;
#endif
}

file_t *
ex_file_new(void)
{
	struct file *file;
	int r;

	r = obj_new(&file, file_class, sizeof(struct file), NULL);
	kassert(r == 0);
	ke_mutex_init(&file->offset_mutex);
	file->offset = 0;

	return file;
}

ex_desc_ret_t
ex_service_file_open(eprocess_t *proc, const char *upath)
{
	char *path;
	namecache_handle_t nch;
	descnum_t descnum;
	int r;

	r = copyin_str(upath, &path);
	if (r != 0)
		return r;

	r = vfs_lookup(root_nch, &nch, path, 0);
#if TRACE_SYSCALLS
	if (r != 0)
		kprintf("Couldn't find <%s>\n", upath);
#endif

	if (r == 0) {
		struct file *file;

		file = ex_file_new();
		file->nch = nch;
		file->vnode = nch.nc->vp;

		descnum = ex_object_space_reserve(proc->objspace, false);
		if (descnum == DESCNUM_NULL)
			return -EMFILE;

		ex_object_space_reserved_insert(proc->objspace, descnum, file);

		return descnum;
	} else
		return r;
}

ex_desc_ret_t
ex_service_file_close(eprocess_t *proc, descnum_t handle)
{
	int r;
	obj_t *old;

	r = ex_object_space_free_index(proc->objspace, handle, &old);
	if (r != 0)
		return r;

	obj_release(old);
	return 0;
}

ex_size_ret_t
ex_service_file_read_cached(eprocess_t *proc, descnum_t handle, vaddr_t ubuf,
    size_t count)
{
	void *obj;
	struct file *file;
	io_result_t ret;
	int r;

	obj = ex_object_space_lookup(proc->objspace, handle);
	if (obj == NULL)
		return -EBADF;

	file = obj;
	if (file->vnode->ops->cached_read != NULL) {
		ke_wait(&file->offset_mutex, "offset_mutex", false, false, -1);
		ret = file->vnode->ops->cached_read(file->vnode, ubuf,
		    file->offset, count);
		kassert(ret.result == 0);
		r = ret.count;
		file->offset += r;
		ke_mutex_release(&file->offset_mutex);
	} else {
		r = -1;
	}
	kassert(r >= 0);
	obj_release(file);

	return r;
}

ex_size_ret_t
ex_service_file_write_cached(eprocess_t *proc, descnum_t handle, vaddr_t ubuf, size_t count)
{
	void *obj;
	struct file *file;
	io_result_t ret;
	int r;

	obj = ex_object_space_lookup(proc->objspace, handle);
	if (obj == NULL)
		return -EBADF;

	file = obj;
	if (file->vnode->ops->cached_write != NULL) {
		ke_wait(&file->offset_mutex, "offset_mutex", false, false, -1);
		ret = file->vnode->ops->cached_write(file->vnode, ubuf,
		    file->offset, count);
		kassert(ret.result == 0);
		r = ret.count;
		ke_mutex_release(&file->offset_mutex);
	} else {
		r = -1;
	}
	kassert(r >= 0);
	file->offset += r;
	obj_release(file);

	return r;
}

ex_off_ret_t
ex_service_file_seek(eprocess_t *proc, descnum_t handle, io_off_t offset,
    int whence)
{
	void *obj;
	struct file *file;
	io_off_t new_offset;
	int r;

	obj = ex_object_space_lookup(proc->objspace, handle);
	if (obj == NULL)
		return -EBADF;

	file = obj;
	ke_wait(&file->offset_mutex, "offset_mutex", false, false, -1);

	switch (whence) {
	case SEEK_SET:
		new_offset = offset;
		break;

	case SEEK_CUR:
		new_offset = file->offset + offset;
		break;

	default:
		kfatal("Unimplemented\n");
	}

	r = file->vnode->ops->seek(file->vnode, file->offset, &new_offset);
	if (r != 0) {
		file->offset = new_offset;
	}

	ke_mutex_release(&file->offset_mutex);
	obj_release(file);

	return r;
}
