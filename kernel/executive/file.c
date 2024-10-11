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
#include "kdk/poll.h"
#include "kdk/vfs.h"
#include "object.h"

/* objman.c */
extern obj_class_t file_class;

void
ex_file_free(obj_t *obj)
{
	file_t *file = obj;

	if (!LIST_EMPTY(&file->epoll_watches))
		poll_watched_file_did_close(file);

	if (file->nch.nc != NULL)
		nchandle_release(file->nch);
	else
	 	vn_release(file->vnode);
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
	int r, len;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

	r = vfs_lookup(root_nch, &nch, path, 0);
#if TRACE_SYSCALLS
	if (r != 0)
		kprintf("Couldn't find <%s>\n", upath);
#endif

	kmem_free(path, len + 1);

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

	kprintf("EX_SERVICE_FILE_READ_CACHED %d\n", handle);

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

ex_err_ret_t
ex_service_file_stat(eprocess_t *proc, int fd, const char *path, int flags,
    struct stat *sb)
{
	void *obj;
	struct file *file;
	vattr_t vattr;
	int r;

	if (fd == AT_FDCWD) {
		namecache_handle_t nch;

		r = vfs_lookup(root_nch, &nch, path, 0);
		if (r != 0)
			return r;

		r = nch.nc->vp->ops->getattr(nch.nc->vp, &vattr);
	} else {
		kassert(fd >= 0);
		kassert(flags & AT_EMPTY_PATH || *path == '\0');

		obj = ex_object_space_lookup(proc->objspace, fd);
		if (obj == NULL)
			return -EBADF;

		file = obj;
		r = file->vnode->ops->getattr(file->vnode, &vattr);
		obj_release(file);
	}

	if (r != 0)
		return r;

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
	case VITER_MARKER:
		kfatal("Should be unreachable!\n");
	}

	sb->st_size = vattr.size;
	sb->st_blocks = ROUNDUP(vattr.size, 512) / 512;
	sb->st_blksize = 512;
	sb->st_atim = vattr.atim;
	sb->st_ctim = vattr.ctim;
	sb->st_mtim = vattr.mtim;
	sb->st_ino = vattr.fileid;
	sb->st_dev = vattr.fsid;

	return 0;
}

ex_size_ret_t
ex_service_file_ioctl(eprocess_t *proc, descnum_t handle, int request,
    void *arg)
{
	void *obj;
	struct file *file;
	int r;

	obj = ex_object_space_lookup(proc->objspace, handle);
	if (obj == NULL)
		return -EBADF;

	file = obj;
	if (file->vnode->ops->ioctl == NULL) {
		obj_release(file);
		return -ENOSYS;
	}
	r = file->vnode->ops->ioctl(file->vnode, request, arg);
	obj_release(file);

	return r;
}
