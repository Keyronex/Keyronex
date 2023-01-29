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
 * (~) invariant while referenced
 */
typedef struct vnode {
	// mutex_t	     lock;
	objectheader_t hdr;
	bool	       isroot;
	vtype_t	       type;
	vm_object_t   *vmobj;  /* page cache */
	void	      *data;   /* fs-private data */
	vfs_t	      *vfsp;   /* vfs to which this vnode belongs */
	vfs_t *vfsmountedhere; /* if a mount point, vfs mounted over it */
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
	 * @param dvn LOCKED directory vnode
	 * @param out [out] resultant vnode (add a ref for caller)
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
	 * @param dvn LOCKED directory vnode
	 * @param out [out] resultant vnode (add a ref for caller)
	 * @param name filename
	 */
	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);

	/*!
	 * Open a vnode. This may yield a different vnode.
	 */
	int (*open)(vnode_t *vn, vnode_t **out, int mode);

	/*!
	 * Read uncached from a vnode.
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
	 * Write uncached to a vnode.
	 */
	int (*write)(vnode_t *vn, void *buf, size_t nbyte, off_t off);
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
 * \p out. Caller holds a reference to \p out thereafter.
 * @param attr for the lookup modes that create, the mode to set.
 * If \p last2 is set, then returns the second-to-last component of the path.
 */
int vfs_lookup(vnode_t *cwd, vnode_t **out, const char *path,
    enum lookup_flags flags, vattr_t *attr);

/**
 * Read from @locked \p vn \p nbyte bytes at offset \p off into buffer \p buf.
 */
int vfs_read(vnode_t *vn, void *buf, size_t nbyte, off_t off);

/**
 * Read into @locked \p vn \p nbyte bytes at offset \p off from buffer \p buf.
 */
int vfs_write(vnode_t *vn, void *buf, size_t nbyte, off_t off);

/*! the root filesystem; this will be a tmpfs */
extern vfs_t root_vfs;
/*! root vnode of root_vfs */
extern vnode_t *root_vnode;
/* vnode of /dev */
extern vnode_t *dev_vnode;

#endif /* VFS_H_ */
