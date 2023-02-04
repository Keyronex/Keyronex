/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * Copyright 2020-2022 NetaScale Systems Ltd.
 * All rights reserved.
 */

/*!
 * @file vfs.h
 * @brief Definitions for the Virtual Filesystem Switch. Design based on the
 * SunOS VFS as described in Kleiman (1986).
 */

#ifndef VFS_H_
#define VFS_H_

#include <sys/types.h>

#include <kern/obj.h>
#include <vm/vm.h>

#include "posix/event.h"

struct knote;
struct proc;
struct specnode;
struct stat;
struct vnops;
struct vfsops;
typedef enum vtype { VNON, VREG, VDIR, VCHR } vtype_t;
typedef struct vattr vattr_t;
typedef struct vfs   vfs_t;
typedef struct vnode vnode_t;
typedef struct file  file_t;

typedef struct vattr {
	vtype_t type;
	mode_t	mode;
	size_t	size;
	dev_t	rdev; /*! device represented by file */
} vattr_t;

/*!
 * (fs) private to the filesystem
 * (~) invariant while referenced
 * (m) TODO mount lock
 */
typedef struct vnode {
	objectheader_t hdr;
	/*! (~) is it the root of a filesystem? */
	bool isroot;
	/*! (~) type of vnode */
	vtype_t type;
	/*! (~) page cache; usually a regular vm_object_t, for tmpfs, an anon */
	vm_object_t *vmobj;
	/*! (fs) fs-private data */
	void *data;
	/*! (m) to which vfs does this vnode belong? */
	vfs_t *vfsp;
	/*! (m) if a mountpoint, the vfs mounted over it */
	vfs_t *vfsmountedhere;
	/*! (~) vnode ops vector */
	const struct vnops *ops;
	union {
		struct {
			struct specdev	 *specdev;
			LIST_ENTRY(vnode) spec_list; /** specdev::vnodes */
		};
	};
} vnode_t;

typedef struct vfs {
	TAILQ_ENTRY(vfs)     list;
	const struct vfsops *ops;
	vnode_t		    *vnodecovered; /* vnode this fs is mounted over */
	void		    *data;	   /* fs-private data */
} vfs_t;

/*!
 * Kernel file descriptor.
 */
typedef struct file {
	void	*magic;
	size_t	 refcnt;
	vnode_t *vn;
	size_t	 pos;
} file_t;

struct vnops {
	/**
	 * Create a new vnode in the given directory.
	 *
	 * @param dvn directory vnode
	 * @param out [out] resultant vnode
	 * @param name new file name
	 * @param attr attributes of file (including whether file, directory,
	 * device node...)
	 */
	int (*create)(vnode_t *dvn, vnode_t **out, const char *name,
	    vattr_t *attr);

	/*!
	 * Get attributes.
	 */
	int (*getattr)(vnode_t *vn, vattr_t *out);

	/**
	 * Lookup the vnode corresponding to the given file name in the given
	 * direct vnode.
	 *
	 * @param dvn directory vnode
	 * @param out $returns_retained resultant vnode
	 * @param name filename
	 */
	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);

	/*!
	 * Open a vnode. This may yield a different vnode. In any case, the
	 * result is referenced.
	 *
	 * @param out $returns_retained resultant vnode
	 */
	int (*open)(vnode_t *vn, vnode_t **out, int mode);

	/*!
	 * Read (via cache) from a vnode.
	 */
	int (*read)(vnode_t *vn, void *buf, size_t nbyte, off_t off);

	/*!
	 * Read directory entries into a buffer.
	 *
	 * @returns -errno for an error condition
	 * @returns 0 for no more entries available
	 * @returns >= 1 sequence number
	 */
	int (*readdir)(vnode_t *dvn, void *buf, size_t nbyte, size_t *bytesRead,
	    off_t seqno);

	/*!
	 * Write (via cache) to a vnode.
	 */
	int (*write)(vnode_t *vn, void *buf, size_t nbyte, off_t off);

	/*!
	 * Change poll state on a vnode.
	 * @retval 0 for poll added
	 * @retval 1 for poll immediately satisfied; pollhead was not added
	 */
	int (*chpoll)(vnode_t *vn, struct epollhead *ph, enum chpoll_kind kind);
};

struct vfsops {

	/*!
	 * Mount the filesystem.
	 * @param vfs vfs structure to be mounted; mountee will need to fill in
	 * relevant parts
	 * @param path path to mount at (must point to a directory). If NULL,
	 * the filesystem is mounted as root.
	 * @param data per-filesystem specific data (e.g. )
	 */
	int (*mount)(vfs_t *vfs, const char *path, void *data);

	/*!
	 * Get the root vnode of the filesystem.
	 */
	int (*root)(vfs_t *vfs, vnode_t **out);

	/*!
	 * Get the vnode corresponding to the given inode number.
	 */
	int (*vget)(vfs_t *vfs, vnode_t **out, ino_t inode);
};

enum lookup_flags {
	kLookupCreat = 1 << 0,
	kLookupMustDir = 1 << 1,
};

/**
 * Lookup path \p path relative to @locked \p cwd and store the result in
 * \p out. (Caller holds a reference to \p out thereafter.)
 * @param attr for the lookup modes that create, the mode to set.
 * If \p last2 is set, then returns the second-to-last component of the path.
 */
int vfs_lookup(vnode_t *cwd, vnode_t **out, const char *path,
    enum lookup_flags flags, vattr_t *attr);

#define VOP_OPEN(vnode, out, mode) vnode->ops->open(vnode, out, mode)
#define VOP_READ(vnode, buf, nbyte, off) \
	vnode->ops->read(vnode, buf, nbyte, off)
#define VOP_WRITE(vnode, buf, nbyte, off) \
	vnode->ops->write(vnode, buf, nbyte, off)
#define VOP_CREAT(vnode, out, name, attr) \
	vnode->ops->create(vnode, out, name, attr)
#define VOP_LOOKUP(vnode, out, path) vnode->ops->lookup(vnode, out, path)
#define VOP_MKDIR(vnode, out, name, attr) \
	vnode->ops->mkdir(vnode, out, name, attr)
#define VOP_POLL(VNODE, POLLHEAD, CHPOLL_KIND) \
	(VNODE)->ops->chpoll(VNODE, POLLHEAD, CHPOLL_KIND)

/*! the root filesystem; this will be a tmpfs */
extern vfs_t root_vfs;
/*! root vnode of root_vfs */
extern vnode_t *root_vnode;
/* vnode of /dev */
extern vnode_t *dev_vnode;

#endif /* VFS_H_ */
