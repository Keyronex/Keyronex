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

	vn = vn_retain(result.nc->vp);

	if (result.nc->vp->ops->open != NULL) {
		r = VOP_OPEN(&vn, flags);
		if (r != 0) {
			nchandle_release(result);
			return r;
		}

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
		} else {
			/*
			 * The namecache handle is kept, and that retains the
			 * vnode. file_release() will only release the namecache
			 * handle. So release the extra retain on vn.
			 */
			vn_release(vn);
		}

	}

	file = file_new(result, vn, flags);
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

/*
 * given a starting point and a path, resolve it up to the penultimate
 * component, return the final component
 *
 * retained penultimate component handle written to parent_nch
 * final component pointer (within path) written to basename
 */
static int
lookup_parent_and_basename(namecache_handle_t start, const char *path,
    enum lookup_flags flags, namecache_handle_t *parent_nch,
    const char **basename)
{
	struct lookup_info info;
	const char *slash, *name;
	int r;

	/* find the last component */
	slash = strrchr(path, '/');
	if (slash == NULL) {
		/* e.g. "file" */
		name = path;
		*parent_nch = nchandle_retain(start);
	} else if (slash == path) {
		/* e.g. "/" */
		name = slash + 1;
		*parent_nch = nchandle_retain(root_nch);
	} else {
		/* e.g. "dir/foo" - need to lookup parent */
		size_t dirlen = slash - path;
		char *dirpath = kmem_alloc(dirlen + 1);
		if (dirpath == NULL)
			return -ENOMEM;

		memcpy(dirpath, path, dirlen);
		dirpath[dirlen] = '\0';
		name = slash + 1;

		r = vfs_lookup_init(&info, start, dirpath, flags);
		if (r != 0) {
			kmem_free(dirpath, dirlen + 1);
			return r;
		}

		r = vfs_lookup(&info);
		kmem_free(dirpath, dirlen + 1);

		if (r != 0)
			return r;

		if (info.result.nc->vp->type != VDIR) {
			nchandle_release(info.result);
			return -ENOTDIR;
		}

		*parent_nch = info.result;
	}

	/* basename musn't be empty */
	if (*name == '\0') {
		nchandle_release(*parent_nch);
		return -ENOENT;
	}

	*basename = name;
	return 0;
}

int
sys_faccessat(int dirfd, const char *upath, int mode, int flags)
{
	char *path;
	namecache_handle_t dirnch, result;
	int r, len;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

	r = get_dirfd_nch(dirfd, &dirnch);
	if (r != 0) {
		kmem_free(path, len + 1);
		return r;
	}

	r = vfs_lookup_simple(dirnch, &result, path,
	    (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW_FINAL : 0);
	if (r != 0) {
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return r;
	}

	/* ... permission check ... */

	kmem_free(path, len + 1);
	nchandle_release(result);
	nchandle_release(dirnch);

	return 0;
}

int
sys_mkdirat(int dirfd, const char *upath, mode_t mode)
{
	char *path;
	namecache_handle_t dirnch;
	struct lookup_info info;
	vattr_t create_attr;
	int r, len;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

	if (path[0] == '\0') {
		kmem_free(path, len + 1);
		return -ENOENT;
	}

#if TRACE_SYSCALLS
	kprintf("sys_mkdirat (%s): dirfd=%d path='%s' mode=0o%o\n",
	    curproc()->comm, dirfd, path, mode);
#endif

	r = get_dirfd_nch(dirfd, &dirnch);
	if (r != 0) {
		kmem_free(path, len + 1);
		return r;
	}

	r = vfs_lookup_init(&info, dirnch, path, 0);
	if (r != 0) {
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return r;
	}

	memset(&create_attr, 0, sizeof(create_attr));
	create_attr.type = VDIR;
	create_attr.mode = mode & ~S_IFMT;
	/* TODO: umask here */

	info.flags |= LOOKUP_CREATE;
	info.create_attr = &create_attr;

	r = vfs_lookup(&info);
	nchandle_release(dirnch);
	kmem_free(path, len + 1);
	if (r != 0)
		return r;

	if (!info.did_create) {
		nchandle_release(info.result);
		return -EEXIST;
	}

	nchandle_release(info.result);

	return 0;
}

int
sys_linkat(int olddirfd, const char *uoldpath, int newdirfd,
    const char *unewpath, int flags)
{
	char *oldpath, *newpath;
	namecache_handle_t old_dirnch, new_dirnch;
	namecache_handle_t target_nch, new_parent_nch;
	const char *newname;
	int r, oldlen, newlen;

	oldlen = strldup_user(&oldpath, uoldpath, 4095);
	if (oldlen < 0)
		return oldlen;

	newlen = strldup_user(&newpath, unewpath, 4095);
	if (newlen < 0) {
		kmem_free(oldpath, oldlen + 1);
		return newlen;
	}

#if TRACE_SYSCALLS
	kprintf("sys_linkat: olddirfd=%d oldpath='%s' newdirfd=%d "
	    "newpath='%s' flags=0x%x\n",
	    olddirfd, oldpath, newdirfd, newpath, flags);
#endif

	/* get the starting directories */
	r = get_dirfd_nch(olddirfd, &old_dirnch);
	if (r != 0)
		goto out_free_paths;

	r = get_dirfd_nch(newdirfd, &new_dirnch);
	if (r != 0)
		goto out_release_old_dir;

	/* look up the source file */
	r = vfs_lookup_simple(old_dirnch, &target_nch, oldpath,
	    (flags & AT_SYMLINK_NOFOLLOW) ? LOOKUP_NOFOLLOW_FINAL : 0);
	if (r != 0)
		goto out_release_new_dir;

	/* can't hard link directories */
	if (target_nch.nc->vp->type == VDIR) {
		r = -EPERM;
		goto out_release_target;
	}

	/* look up the new parent directory */
	r = lookup_parent_and_basename(new_dirnch, newpath, 0,
	    &new_parent_nch, &newname);
	if (r != 0)
		goto out_release_target;

	r = nc_link(new_parent_nch, target_nch.nc->vp, newname);

	nchandle_release(new_parent_nch);
out_release_target:
	nchandle_release(target_nch);
out_release_new_dir:
	nchandle_release(new_dirnch);
out_release_old_dir:
	nchandle_release(old_dirnch);
out_free_paths:
	kmem_free(newpath, newlen + 1);
	kmem_free(oldpath, oldlen + 1);


	return r;
}

int
sys_unlinkat(int dirfd, const char *upath, int flags)
{
	char *path;
	namecache_handle_t dirnch, parent_nch;
	const char *name;
	bool isdir;
	int r, len;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

	isdir = (flags & AT_REMOVEDIR) != 0;

#if TRACE_SYSCALLS
	kprintf("sys_unlinkat: dirfd=%d path='%s' flags=0x%x\n",
	    dirfd, path, flags);
#endif

	r = get_dirfd_nch(dirfd, &dirnch);
	if (r != 0) {
		kmem_free(path, len + 1);
		return r;
	}

	r = lookup_parent_and_basename(dirnch, path, 0, &parent_nch, &name);
	if (r != 0) {
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return r;
	}

	/* can't remove "." or ".." */
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		r = -EINVAL;
		goto out;
	}

	r = nc_remove(parent_nch, name, isdir);

out:
	nchandle_release(parent_nch);
	nchandle_release(dirnch);
	kmem_free(path, len + 1);

	return r;
}

int
sys_renameat(int olddirfd, const char *uoldpath, int newdirfd,
    const char *unewpath)
{
	char *oldpath, *newpath;
	namecache_handle_t old_dirnch, new_dirnch;
	namecache_handle_t old_parent_nch, new_parent_nch;
	const char *oldname, *newname;
	int r, oldlen, newlen;

	oldlen = strldup_user(&oldpath, uoldpath, 4095);
	if (oldlen < 0)
		return oldlen;

	newlen = strldup_user(&newpath, unewpath, 4095);
	if (newlen < 0) {
		kmem_free(oldpath, oldlen + 1);
		return newlen;
	}

#if TRACE_SYSCALLS
	kprintf("sys_renameat: olddirfd=%d oldpath='%s' newdirfd=%d "
	    "newpath='%s'\n",
	    olddirfd, oldpath, newdirfd, newpath);
#endif

	/* get the starting directories */
	r = get_dirfd_nch(olddirfd, &old_dirnch);
	if (r != 0)
		goto out_free_paths;

	r = get_dirfd_nch(newdirfd, &new_dirnch);
	if (r != 0)
		goto out_release_old_dir;

	/* look up the old parent directory */
	r = lookup_parent_and_basename(old_dirnch, oldpath, 0,
	    &old_parent_nch, &oldname);
	if (r != 0)
		goto out_release_new_dir;

	/* can't rename "." or ".." */
	if (strcmp(oldname, ".") == 0 || strcmp(oldname, "..") == 0) {
		r = -EINVAL;
		goto out_release_old_parent;
	}

	/* look up the new parent directory */
	r = lookup_parent_and_basename(new_dirnch, newpath, 0,
	    &new_parent_nch, &newname);
	if (r != 0)
		goto out_release_old_parent;

	/* can't rename to "." or ".." */
	if (strcmp(newname, ".") == 0 || strcmp(newname, "..") == 0) {
		r = -EINVAL;
		goto out_release_new_parent;
	}

	r = nc_rename(old_parent_nch, oldname, new_parent_nch, newname);

out_release_new_parent:
	nchandle_release(new_parent_nch);
out_release_old_parent:
	nchandle_release(old_parent_nch);
out_release_new_dir:
	nchandle_release(new_dirnch);
out_release_old_dir:
	nchandle_release(old_dirnch);
out_free_paths:
	kmem_free(newpath, newlen + 1);
	kmem_free(oldpath, oldlen + 1);

	return r;
}

int
sys_readlinkat(int dirfd, const char *upath, char *ubuf, size_t bufsiz)
{
	char *path;
	namecache_handle_t dirnch, result;
	int r, len;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

	r = get_dirfd_nch(dirfd, &dirnch);
	if (r != 0) {
		kmem_free(path, len + 1);
		return r;
	}

	r = vfs_lookup_simple(dirnch, &result, path, LOOKUP_NOFOLLOW_FINAL);
	if (r != 0) {
		nchandle_release(dirnch);
		kmem_free(path, len + 1);
		return r;
	}

	if (result.nc->vp->type != VLNK) {
		r = -EINVAL;
		goto out;
	}

	r = VOP_READLINK(result.nc->vp, ubuf, bufsiz);

out:
	nchandle_release(result);
	nchandle_release(dirnch);
	kmem_free(path, len + 1);

	return r;
}

int
sys_fchmodat(int dirfd, const char *pathname, mode_t mode, int flags)
{
	kdprintf("warnings: sys_fchmodat() is a noop!\n");
	return 0;
}

int
sys_fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group,
    int flags)
{
	kdprintf("warnings: sys_fchownat() is a noop!\n");
	return 0;
}

int
sys_truncate(const char *upath, off_t length)
{
	char *path;
	namecache_handle_t nch;
	int r, len;
	vattr_t attr;

	len = strldup_user(&path, upath, 4095);
	if (len < 0)
		return len;

	r = vfs_lookup_simple(root_nch, &nch, path, 0);
	if (r != 0) {
		kmem_free(path, len + 1);
		return r;
	}

	if (nch.nc->vp->type != VREG) {
		r = -EINVAL;
		goto out;
	}

	attr = VATTR_NULL;
	attr.size = length;
	r = VOP_SETATTR(nch.nc->vp, &attr);

out:
	nchandle_release(nch);
	kmem_free(path, len + 1);

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
sys_getdents(int fd, void *buf, size_t nbyte)
{
	file_t *file;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	ke_mutex_enter(&file->offset_mutex, "sys_getdents");
	kassert(file->vnode->ops->readdir != NULL);
	r = VOP_READDIR(file->vnode, buf, nbyte, &file->offset);
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

	offset = (off_t)(int32_t)offset; /* hack for 32-bit arches */

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
sys_ioctl(int fd, unsigned long cmd, intptr_t arg)
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

		kassert(nch.nc->vp->ops->getattr != NULL);
		r = VOP_GETATTR(nch.nc->vp, &vattr);
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

int
sys_ftruncate(int fd, off_t length)
{
	file_t *file;
	vattr_t attr;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	if (file->vnode->type != VREG) {
		r = -EINVAL;
		goto out;
	}

	attr = VATTR_NULL;
	attr.size = length;
	r = VOP_SETATTR(file->vnode, &attr);

out:
	file_release(file);
	return r;
}

int
sys_flock(int fd, int op)
{
	file_t *file;
	int r;

	file = uf_lookup(curproc()->finfo, fd);
	if (file == NULL)
		return -EBADF;

	kdprintf("sys_flock: fd=%d op=0x%x is a no-op!\n", fd, op);
	r = 0;

out:
	file_release(file);
	return r;
}
