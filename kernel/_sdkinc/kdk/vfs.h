/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 02 2023.
 */

#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <sys/types.h>

#include <stddef.h>
#include <stdint.h>

#include "./object.h"
#include "kdk/objhdr.h"
#include "kdk/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum vtype { VNON, VREG, VDIR, VCHR, VLNK } vtype_t;

typedef struct vattr {
	vtype_t type;
	mode_t mode;
	size_t size;
	/*! device represented by file */
	dev_t rdev;
} vattr_t;

/*!
 * (~) invariant from initialisation
 * (m) mount lock (todo)
 */
typedef struct vnode {
	object_header_t objhdr;

	/*! (~) operations */
	struct vnops *ops;
	/*! (~) type of vnode */
	vtype_t type;
	/*! (~) section object; one reference held */
	vm_section_t *section;
	/*! (~) mountpoint to which the vnode belongs */
	struct vfs *vfsp;
	/*! (~) whether this vnode is the root of its mountpoint */
	bool isroot;
	/*! (m) vfs mounted over this vnode */
	struct vfs *vfsmountedhere;
	/*! (fs-dependent) fs private data */
	uintptr_t data;
} vnode_t;

/*!
 * Per-mountpoint structure.
 */
typedef struct vfs {
	/*! vnode over which the mount was made, if not root. */
	vnode_t *vnodecovered;
	/*! filesystem ops */
	struct vfsops *ops;
	/*! the device object of the filesystem (n.b. NOT dev mounted on) */
	struct device *dev;
	/*! fs-private data */
	uintptr_t data;
} vfs_t;

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
	 * @param out resultant vnode
	 * @param name filename
	 */
	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);

	/*!
	 * Open a vnode. This may change the vnode.
	 */
	int (*open)(krx_inout vnode_t **vn, int mode);

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
	 * @brief Read a symlink's target.
	 */
	int (*readlink)(vnode_t *dvn, char *out);

	/*!
	 * Write (via cache) to a vnode.
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
	 * @param data per-filesystem specific data
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
	kLookupCreate = 1 << 0,
	kLookupFollowSymlinks = 1 << 1,
	kLookupMustDir = 1 << 2,
};

/*! Initialises the master DevFS. */
int vfs_mountdev1(void);

int vfs_lookup(vnode_t *cwd, vnode_t **out, const char *pathname,
    enum lookup_flags flags, vattr_t *attr);

/*! VFS of the master DevFS */
extern vfs_t dev_vfs;
/*! Root vnode of the master DevFS */
extern vnode_t *dev_vnode;
/*! Root vnode of the root filesystem. */
extern vnode_t *root_vnode;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_KDK_VFS_H */
