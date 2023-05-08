/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Mar 02 2023.
 */

#ifndef KRX_KDK_VFS_H
#define KRX_KDK_VFS_H

#include <sys/types.h>
#include <sys/statfs.h>

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "./object.h"
#include "kdk/objhdr.h"
#include "kdk/vm.h"

#ifdef __cplusplus
extern "C" {
#endif

struct pollhead;

enum chpoll_kind {
	/*! poll is being added; the pollhead should NOT exist in the list */
	kChPollAdd,
	/*! poll is being removed; the pollhead MUST exist in the list */
	kChPollRemove,
	/*! poll conditions are being changed */
	kChPollChange
};

typedef enum vtype { VNON, VREG, VDIR, VCHR, VLNK, VSOCK, VFIFO } vtype_t;

typedef struct vattr {
	vtype_t type;
	mode_t mode;
	/*! note: can become stale with respect to vnode::size */
	size_t size;
	/* access, modification, creation times */
	struct timespec atim, mtim, ctim;
	/* unique identifier */
	ino_t ino;
	/*! legacy device represented by file */
	dev_t rdev;
	/*! new-style device represented by file */
	struct device *rdevice;
	/*! new-style device operations vector */
	struct vnops *rdevops;
} vattr_t;

/*!
 * For pgcache_read()/pgcache_write(), the vnode lock is acquired to prevent
 * changes in size during the operation. The flag `locked_for_paging` is set to
 * indicate this, so that if a page fault associated with the pgcache operation
 * must do paging I/O, which would typically involve acquiring the vnode lock,
 * this can be elided to eliminate a lock ordering problem.
 *
 * (~) invariant from initialisation
 * (m) mount lock (todo)
 * (l) vnode->lock
 */
typedef struct vnode {
	/*! (~) */
	vm_object_t vmobj;

	bool
	    /* (l) vnode is locked for paging I/O */
	    locked_for_paging : 1,
	    isroot : 1;

	/*! the vnode read/wriet lock */
	kmutex_t lock;

	/*! (~) operations */
	struct vnops *ops;
	/*! (~) type of vnode */
	vtype_t type;

	/*! (l) size of regular file */
	size_t size;

	/*! (~) mountpoint to which the vnode belongs */
	struct vfs *vfsp;
	/*! (m) vfs mounted over this vnode */
	struct vfs *vfsmountedhere;
	/*! (fs-dependent) fs private data */
	uintptr_t data;
	/*! (fs-dependent) fs private data 2 */
	uintptr_t data2;

	union {
		struct {
			/*! (~) device */
			struct device *rdevice;
			/*! (~) device ops */
			struct vnops *rdeviceops;
		};

		struct sock_unix *sock;
	};
} vnode_t;

/*!
 * Per-mountpoint structure. most fields are stable from creation onwards, except
 * refcnt, which is guarded by the mount_lock.
 */
typedef struct vfs {
	/*! (m) linkage in vfs_tailq */
	TAILQ_ENTRY(vfs) tailq_entry;
	/*! (m) reference count - unmountable till = 1 */
	size_t refcnt;
	/*! vnode over which the mount was made, if not root. */
	vnode_t *vnodecovered;
	/*! filesystem ops */
	struct vfsops *ops;
	/*! the device object of the filesystem (n.b. NOT dev mounted on) */
	struct device *dev;
	/*! fs-private data */
	uintptr_t data;

	/* device name or similar */
	const char *devname;
	/* mountpoint */
	const char *mountpoint;
	/* fs type */
	const char *type;
} vfs_t;

struct vnops {
	/*!
	 * @brief Open a vnode.
	 *
	 * @param vn Pointer to pointer to vnode to open. Opening may yield a
	 * different vnode, in which case the vnode is released and the pointer
	 * to the vnode resultant from opening is written out.
	 * @param mode Mode to open the file with.
	 */
	int (*open)(krx_inout vnode_t **vn, int mode);
	/*!
	 * @brief Close an open vnode.
	 *
	 * @param vn vnode to close.
	 */
	int (*close)(vnode_t *vn);
	/*!
	 * @brief Read (via page cache) from a vnode.
	 */
	int (*read)(vnode_t *vn, void *buf, size_t nbyte, off_t off);
	/*!
	 * @brief Write (via page cache) to a vnode.
	 */
	int (*write)(vnode_t *vn, void *buf, size_t nbyte, off_t off);
	/*!
	 * @brief I/O control
	 */
	int (*ioctl)(vnode_t *vn, unsigned long command, void *data);
	/*!
	 * @brief Get vnode attributes.
	 */
	int (*getattr)(vnode_t *vn, vattr_t *out);

	/*!
	 * Lookup the vnode corresponding to the given file name in the given
	 * direct vnode.
	 *
	 * @param dvn directory vnode
	 * @param out resultant vnode
	 * @param name filename
	 */
	int (*lookup)(vnode_t *dvn, vnode_t **out, const char *name);
	/*!
	 * Create a new file/socket in the given directory.
	 *
	 * @param dvn directory vnode
	 * @param out [out] resultant vnode
	 * @param name new file name
	 * @param attr system buffer for attributes of file to be created
	 * (including whether file, directory, device node...)
	 */
	int (*create)(vnode_t *dvn, krx_out vnode_t **out, const char *name,
	    vattr_t *attr);
	/*!
	 * @brief Remove a link from a directory.
	 */
	int (*remove)(vnode_t *dvn, const char *name);
	/*!
	 * @brief Create a hardlink.
	 *
	 * @param dvn Directory vnode to make the link in.
	 * @param vn vnode to establish the link to.
	 * @param name Name of link to create in the directory.
	 */
	int (*link)(vnode_t *dvn, vnode_t *vn, const char *name);
	/*!
	 * @brief Rename a directory entry, possibly moving to a new directory.
	 *
	 * @brief old_dvn Directory vnode where the original entry is held.
	 * @brief old_name Name of the entry in old_dvn
	 * @brief new_dvn Directory vnode the entry is to be moved into.
	 * @brief new_name Name of the entry to create in new_dvn.
	 */
	int (*rename)(vnode_t *old_dvn, const char *old_name, vnode_t *new_dvn,
	    const char *new_name);
	/*!
	 * @brief Create a new directory in the given directory.
	 *
	 * @brief vn Directory vnode to create a directory in.
	 * @brief out Newly created directory vnode is written out here.
	 * @brief name Name of the new directory to create.
	 * @brief attr Attributes of new directory to create.
	 */
	int (*mkdir)(vnode_t *vn, vnode_t **out, const char *name,
	    vattr_t *attr);
	/*!
	 * @brief Delete the named directory from a given directory.
	 *
	 * @brief vn Directory vnode to remove the directory from.
	 * @brief name Name of the directory to delete.
	 */
	int (*rmdir)(vnode_t *vn, const char *name);
	/*!
	 * @brief Read directory entries into a system buffer.
	 *
	 * @returns -errno for an error condition
	 * @retval 0	   no more entries available
	 * @retval other   sequence number
	 */
	off_t (*readdir)(vnode_t *dvn, void *buf, size_t nbyte,
	    size_t *bytesRead, off_t seqno);

	/*!
	 * @brief Create a symlink.
	 */
	int (*symlink)(vnode_t *vn, vnode_t **out, const char *name,
	    vattr_t *attr, const char *target);
	/*!
	 * @brief Read a symlink's target.
	 */
	int (*readlink)(vnode_t *dvn, char *out);

	/*!
	 * @brief Change poll state.
	 */
	int (*chpoll)(vnode_t *vn, struct pollhead *, enum chpoll_kind);
	/*!
	 * @brief Map vnode into memory (see vm_map_object)
	 */
	int (*mmap)(vnode_t *vn, vm_map_t *map, krx_inout vaddr_t *vaddrp,
	    size_t size, voff_t offset, vm_protection_t initial_protection,
	    vm_protection_t max_protection, enum vm_inheritance inheritance,
	    bool exact, bool copy);
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
	int (*mount)(vfs_t *vfs, vnode_t *over, void *data);
	/*!
	 * Get the root vnode of the filesystem.
	 */
	int (*root)(vfs_t *vfs, vnode_t **out);
	/*!
	 * Get FS info.
	 */
	int (*statfs)(vfs_t *vfs, struct statfs *out);
	/*!
	 * Get the vnode corresponding to the given inode number.
	 */
	int (*vget)(vfs_t *vfs, vnode_t **out, ino_t inode);
};

enum lookup_flags {
	kLookupNoFollowFinalSymlink = 1 << 1,
	kLookup2ndLast = 1 << 2,
};

#define DIRENT_RECLEN(NAMELEN) \
	ROUNDUP(offsetof(struct dirent, d_name[0]) + 1 + NAMELEN, 8)

#define VOP_IOCTL(VN, CMD, DATA) (VN)->ops->ioctl(VN, CMD, DATA)
#define VOP_LINK(DVN, VN, NAME) (VN)->ops->link(DVN, VN, NAME)
#define VOP_OPEN(VN, mode) (*VN)->ops->open(VN, mode)
#define VOP_READ(vnode, buf, nbyte, off) \
	vnode->ops->read(vnode, buf, nbyte, off)
#define VOP_READDIR(VN, BUF, BUFSIZE, PBYTES_READ, SEQNO) \
	(VN)->ops->readdir(VN, BUF, BUFSIZE, PBYTES_READ, SEQNO)
#define VOP_READLINK(PVN, PBUF) (PVN)->ops->readlink(PVN, PBUF)
#define VOP_WRITE(vnode, buf, nbyte, off) \
	vnode->ops->write(vnode, buf, nbyte, off)
#define VOP_CREAT(vnode, out, name, attr) \
	vnode->ops->create(vnode, out, name, attr)
#define VOP_REMOVE(VN, NAME) (VN)->ops->remove(VN, NAME)
#define VOP_GETATTR(VN, OUT) (VN)->ops->getattr(VN, OUT)
#define VOP_LOOKUP(vnode, out, path) vnode->ops->lookup(vnode, out, path)
#define VOP_RENAME(VN, OLD_NAME, NEW_VN, NEW_NAME) \
	(VN)->ops->rename(VN, OLD_NAME, NEW_VN, NEW_NAME)
#define VOP_MKDIR(vnode, out, name, attr) \
	vnode->ops->mkdir(vnode, out, name, attr)
#define VOP_CHPOLL(VN, PH, KIND) (VN)->ops->chpoll(VN, PH, KIND)

enum vtype mode_to_vtype(mode_t mode);

/*! @brief Create a new unnamed device vnode. */
vnode_t *devfs_create_unnamed(void *rdevice, struct vnops *devvnops);
/*! @brief Setup a vnode with devfs vnode ops. */
int devfs_setup_vnode(vnode_t *vn, struct device *rdevice,
    struct vnops *devvnops);

/*! Initialises the master DevFS. */
int vfs_mountdev1(void);

/*!
 * @brief Look up a pathname.
 */
int vfs_lookup(vnode_t *start, vnode_t **out, const char *path,
    enum lookup_flags flags);

/*!
 *  @brief Look up 2nd last part of path and get the remainder of the path.
 */
int vfs_lookup_for_at(vnode_t *vn, vnode_t **dvn_out, const char *path,
    const char **lastpart_out);

/*!
 * \defgroup pgcache Page Cache helpers
 * These operations should not be used directly because they do nothing in terms
 * of locking. They are for filesystems to use from within their read/write
 * vnode ops.
 * @{
 */
/*! @brief Read from the page cache into a user/system buffer. */
int pgcache_read(vnode_t *vn, void *buf, size_t nbyte, off_t off);
/*! @brief Write into the page cache from a user/system buffer. */
int pgcache_write(vnode_t *vn, void *buf, size_t nbyte, off_t off);
/*!
 * @} pgcache
 */

/*! VFS of the master DevFS */
extern vfs_t dev_vfs;
/*! Root vnode of the master DevFS */
extern vnode_t *dev_vnode;
/*! Root vnode of the root filesystem. */
extern vnode_t *root_vnode;
/*! Mount spinlock. */
extern kspinlock_t mount_lock;
/*! All mounts list. */
extern TAILQ_HEAD(vfs_tailq, vfs) vfs_tailq;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KRX_KDK_VFS_H */
