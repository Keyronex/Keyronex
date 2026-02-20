/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2022-26 Cloudarox Solutions.
 */
/*!
 * @file vnode.h
 * @brief Virtual node.
 */

#ifndef ECX_SYS_VNODE_H
#define ECX_SYS_VNODE_H

#include <sys/iop.h>
#include <sys/k_intr.h>
#include <sys/krx_atomic.h>
#include <sys/types.h>

#include <libkern/queue.h>

struct poll_entry;

typedef enum vtype {
	VNON,
	VREG,
	VDIR,
	VBLK,
	VCHR,
	VLNK,
	VSOCK,
	VFIFO,
	VITER_MARKER
} vtype_t;

typedef struct vattr {
	enum vtype	type;	/*!< vnode type */
	mode_t		mode;	/*!< mode and type */
	nlink_t		nlink;	/*!< number of links to file */
	uid_t		uid;	/*!< owning user*/
	gid_t		gid;	/*!< owning group*/
	dev_t		fsid;	/*!< fs unique id */
	ino_t		fileid;	/*!< file unique id */
	uint64_t	size;	/*!< size in bytes */
	uint64_t	bsize;	/*!< fs block size  */
	struct timespec	atim,	/*!< last access time */
			mtim,	/*!< last modified time */
			ctim;	/*!< creation time */
	dev_t 		rdev;	/*!< represented device */
	uint64_t	dsize;	/*!< on-disk size in bytes */
} vattr_t;

typedef struct vnode {
	atomic_uint	refcount;
	struct vfs	*vfs;
	vtype_t		type;
	struct vnode_ops *ops;
	TAILQ_ENTRY(vnode) vfs_vnlistentry; /* entry in vfs::vnode_list */
	union {
		struct {
			struct vn_vc_state	*vc_state;
			struct vm_object	*vmobj;
			kspinlock_t 		dpw_lock;
			size_t 			dpw_writers;
			SLIST_HEAD(, dpw_waiter) dpw_waiters;
		} file;
		struct {
			struct vnode *sockvn;
		} sock;
	};
	uintptr_t	fsprivate_1,
			fsprivate_2;
} vnode_t;

typedef struct vfs_vn_iter {
	vnode_t		*end_marker_vn;
	vnode_t		*next_vn;
	struct vfs	*vfs;
} vfs_vnode_iter_t;

/* What kind of chpoll operation is being done. */
enum chpoll_mode {
	CHPOLL_POLL,
	CHPOLL_UNPOLL,
};

struct vnode_ops {
	size_t stack_depth;
	int (*open)(vnode_t **, int);
	int (*close)(vnode_t *, int);
	int (*create)(vnode_t *, const char *, vattr_t *, vnode_t **);
	int (*link)(vnode_t *, vnode_t *, const char *);
	int (*remove)(vnode_t *, const char *);
	int (*rename)(vnode_t *, const char *, vnode_t *,
	    const char *);
	int (*ioctl)(vnode_t *, unsigned long cmd, void *data);
	int (*getattr)(vnode_t *, vattr_t*);
	int (*setattr)(vnode_t *, vattr_t*);
	int (*lookup)(vnode_t *, const char *, vnode_t **);
	int (*readdir)(vnode_t *, void *buf, size_t length, off_t *offset);
	int (*readlink)(vnode_t *, char *buf, size_t buflen);
	int (*inactive)(vnode_t *);
	int (*read)(vnode_t *, void *buf, size_t length, off_t offset,
	    int flags);
	int (*write)(vnode_t *, const void *buf, size_t length, off_t offset,
	    int flags);
	int (*seek)(vnode_t *, off_t old_offset, off_t *new_offset);
	int (*chpoll)(vnode_t *, struct poll_entry *, enum chpoll_mode);
	int (*mmap)(void *addr, size_t len, int prot, int flags, vnode_t *vn,
	    off_t offset, vaddr_t *window);
	void (*lock_for_vc_io)(vnode_t *, bool write);
	void (*unlock_for_vc_io)(vnode_t *, bool write);
	iop_return_t (*iop_dispatch)(vnode_t *vn, struct iop *);
	iop_return_t (*iop_complete)(vnode_t *vn, struct iop *);
};

#define VOP_OPEN(VN, FLAGS) (*(VN))->ops->open(VN, FLAGS);
#define VOP_RENAME(SVN, SNAME, DVN, DNAME) \
    (SVN)->ops->rename(SVN, SNAME, DVN, DNAME)
#define VOP_CHPOLL(VN, POLL, MODE) (VN)->ops->chpoll(VN, (POLL), (MODE));
#define VOP_READLINK(VN, BUF, LEN) (VN)->ops->readlink(VN, (BUF), (LEN));
#define VOP_GETATTR(VN, VATTR) (VN)->ops->getattr(VN, (VATTR));
#define VOP_IOCTL(VN, CMD, DATA) (VN)->ops->ioctl(VN, (CMD), (DATA));
#define VOP_READ(VN, BUF, LEN, OFF, F) (VN)->ops->read(VN, BUF, LEN, OFF, F);
#define VOP_READDIR(VN, BUF, LEN, OFF) (VN)->ops->readdir(VN, BUF, LEN, OFF);
#define VOP_WRITE(VN, BUF, LEN, OFF, F) (VN)->ops->write(VN, BUF, LEN, OFF, F);
#define VOP_SEEK(VN, OLD, NEW) (VN)->ops->seek(VN, (OLD), (NEW));
#define VOP_LOCK_FOR_VC_IO(VN, WRITE) (VN)->ops->lock_for_vc_io(VN, (WRITE));
#define VOP_UNLOCK_FROM_VC_IO(VN, WRITE) \
    (VN)->ops->unlock_for_vc_io(VN, (WRITE));

vnode_t *vn_alloc(struct vfs *, vtype_t, struct vnode_ops *, uintptr_t fspriv1, uintptr_t fspriv2);

vnode_t *vn_retain(vnode_t *);
void vn_release(vnode_t *);

int viewcache_io(vnode_t *, uint64_t offset, size_t length, bool write,
    void *buf);
struct vn_vc_state *viewcache_alloc_vnode_state(vnode_t *vn);

#endif /* ECX_SYS_VNODE_H */
