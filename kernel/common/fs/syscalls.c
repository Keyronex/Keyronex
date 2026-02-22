/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Sat Feb 21 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file syscalls.c
 * @brief FS-related system calls.
 */

#define _GNU_SOURCE

#include <sys/krx_vfs.h>
#include <sys/krx_file.h>
#include <sys/krx_user.h>
#include <sys/vnode.h>
#include <sys/libkern.h>
#include <sys/kmem.h>
#include <sys/fcntl.h>
#include <sys/proc.h>
#include <sys/errno.h>

static int
get_dirfd_nch(int dirfd, namecache_handle_t *out)
{
	file_t *file;

	if (dirfd == AT_FDCWD) {
		*out = nchandle_retain(root_nch);
		return 0;
	}

	file = uf_lookup(curproc()->finfo, dirfd);
	if (file == NULL)
		return -EBADF;

	if (file->vnode->type != VDIR) {
		file_release(file);
		return -ENOTDIR;
	}

	*out = nchandle_retain(file->nch);
	file_release(file);
	return 0;
}


int
sys_openat(int dirfd, const char *upath, int flags, mode_t mode)
{
	char *path;
	namecache_handle_t dirnch, result;
	file_t *file;
	struct lookup_info info;
	vattr_t create_attr;
	vnode_t *vn;
	int r, len;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

#if TRACE_SYSCALLS
	kprintf("sys_openat (%s): dirfd=%d path='%s' flags=0x%x mode=0o%o\n",
	    curproc()->comm, dirfd, path, flags, mode);
#endif

	if (flags & O_TRUNC)
		kdprintf(" !! sys_openat: O_TRUNC not implemented! (ignored)\n");

	r = get_dirfd_nch(dirfd, &dirnch);
	if (r != 0) {
		kmem_free(path, len + 1);
		return r;
	}

	r = vfs_lookup_init(&info, dirnch, path,
	    (flags & O_NOFOLLOW) ? LOOKUP_NOFOLLOW_FINAL : 0);
	if (r != 0) {
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return r;
	}

	if (flags & O_CREAT) {
		memset(&create_attr, 0, sizeof(create_attr));
		create_attr.type = (flags & O_DIRECTORY) ? VDIR : VREG;
		create_attr.mode = mode & ~S_IFMT;
		/* TODO: umask here */

		info.flags |= LOOKUP_CREATE;
		info.create_attr = &create_attr;
	}

	r = vfs_lookup(&info);
	if (r != 0) {
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return r;
	} else if ((flags & O_CREAT) && (flags & O_EXCL) && !info.did_create) {
		nchandle_release(info.result);
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return -EEXIST;
	}

	result = info.result;
	kmem_free(path, len + 1);
	nchandle_release(dirnch);

	if ((flags & O_DIRECTORY) && result.nc->vp->type != VDIR) {
		nchandle_release(result);
		return -ENOTDIR;
	}

	vn = result.nc->vp;

	if (result.nc->vp->ops->open != NULL) {
		r = VOP_OPEN(&vn, flags);
		if (vn != result.nc->vp) {
			/*
			 * Different vnode (e.g. because of /dev/tty).
			 * So throw away the namecache handle. They aren't
			 * needed but for namespace operations anyway, and you
			 * don't do those on devices.
			 *
			 * FIXME: Pass in the namecache_handle to open?
			 * Then the nc can be released and nulled, or replaced,
			 * as desired by the driver.
			 */
			nchandle_release(result);
			result = NCH_NULL;
		}

	}

	file = file_new(result, vn);
	if (file == NULL) {
		nchandle_release(result);
		return -ENOMEM;
	}

	r = uf_reserve_fd(curproc()->finfo, 0, flags & O_CLOEXEC);
	if (r < 0)
		file_release(file);
	else
		uf_install_reserved(curproc()->finfo, r, file);

	return r;
}

ssize_t
sys_read(int fd, void *ubuf, size_t nbyte)
{
	file_t *file;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	ke_mutex_enter(&file->offset_mutex, "offset_mutex");
	kassert(file->vnode->ops->read != NULL);
	r = VOP_READ(file->vnode, ubuf, nbyte, file->offset, file->flags);
	if (r >= 0)
		file->offset += r;
	ke_mutex_exit(&file->offset_mutex);

	file_release(file);

	return r;
}

ssize_t
sys_write(int fd, const void *ubuf, size_t nbyte)
{
	file_t *file;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	ke_mutex_enter(&file->offset_mutex, "offset_mutex");
	kassert(file->vnode->ops->write != NULL);
	r = VOP_WRITE(file->vnode, ubuf, nbyte, file->offset, file->flags);
	if (r >= 0)
		file->offset += r;
	ke_mutex_exit(&file->offset_mutex);

	file_release(file);

	return r;
}


int
sys_lseek(int fd, off_t offset, int whence, off_t *out)
{
	file_t *file;
	off_t new_off;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	ke_mutex_enter(&file->offset_mutex, "offset_mutex");
	switch (whence) {
	case SEEK_SET:
		new_off = offset;
		break;

	case SEEK_CUR:
		new_off = file->offset + offset;
		break;

	case SEEK_END: {
		vattr_t vattr;
		kassert(file->vnode->ops->getattr != NULL);
		r = VOP_GETATTR(file->vnode, &vattr);
		if (r != 0) {
			ke_mutex_exit(&file->offset_mutex);
			file_release(file);
			return r;
		}
		new_off = vattr.size + offset;
		break;
	}
	default:
		ke_mutex_exit(&file->offset_mutex);
		file_release(file);
		return -EINVAL;
	}
	if (file->vnode->ops->seek == NULL)
		r = -ESPIPE;
	else
		r = VOP_SEEK(file->vnode, file->offset, &new_off);
	if (r == 0) {
		file->offset = new_off;
		*out = new_off;
	}
	ke_mutex_exit(&file->offset_mutex);
	file_release(file);
	return r;
}

int
sys_ioctl(int fd, int cmd, intptr_t arg)
{
	file_t *file;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	kassert(file->vnode->ops->ioctl != NULL);
	r = VOP_IOCTL(file->vnode, (unsigned long)cmd, (void *)arg);
	file_release(file);

	return r;
}

int
sys_fstatat(int fd, const char *upath, int flags, struct stat *sb)
{
	struct file *file;
	vattr_t vattr;
	char *path;
	int lookup_flags = 0;
	int r, pathlen;

	if (flags & AT_SYMLINK_NOFOLLOW)
		lookup_flags |= LOOKUP_NOFOLLOW_FINAL;

	pathlen = strldup_user(&path, upath, 4095);
	if (pathlen < 0)
		return pathlen;

	if (fd == AT_FDCWD) {
		namecache_handle_t nch;

		r = vfs_lookup_simple(root_nch, &nch, path, lookup_flags);
		if (r != 0)
			goto out;

		r = nch.nc->vp->ops->getattr(nch.nc->vp, &vattr);
		nchandle_release(nch);
	} else {
		kassert(fd >= 0);
		kassert(flags & AT_EMPTY_PATH || *path == '\0');

		file = uf_lookup(curproc()->finfo, fd);
		if (file == NULL) {
			r = -EBADF;
			goto out;
		}

		kassert(file->vnode->ops->getattr != NULL);
		r = VOP_GETATTR(file->vnode, &vattr);
		file_release(file);
	}

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

	case VBLK:
		sb->st_mode |= S_IFBLK;
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
		kfatal("Should be unreachable! fd = %d, path = %s\n", fd, path);
	}

	sb->st_size = vattr.size;
	sb->st_blocks = roundup2(vattr.size, 512) / 512;
	sb->st_blksize = 512;
	sb->st_atim = vattr.atim;
	sb->st_ctim = vattr.ctim;
	sb->st_mtim = vattr.mtim;
	sb->st_ino = vattr.fileid;
	sb->st_dev = vattr.fsid;

out:
	if (path != NULL)
		kmem_free(path, pathlen + 1);

	return r;
}
