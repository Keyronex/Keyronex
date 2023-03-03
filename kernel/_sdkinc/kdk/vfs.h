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
	/*! fs-private data */
	uintptr_t data;
} vfs_t;

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

/*! Initialises the master DevFS. */
int vfs_mountdev1(void);

/*! VFS of the master DevFS */
extern vfs_t dev_vfs;
/*! Root vnode of the master DevFS*/
extern vnode_t *dev_vnode;

#endif /* KRX_KDK_VFS_H */
