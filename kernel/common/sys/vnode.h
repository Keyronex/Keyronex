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

#include <sys/types.h>
#include <sys/iop.h>
#include <sys/k_intr.h>
#include <sys/krx_atomic.h>
#include <sys/krx_epoll.h>

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

#define VATTR_NULL (vattr_t) {				\
	.type = VNON,					\
	.mode = -1,					\
	.nlink = -1,					\
	.uid = -1,					\
	.gid = -1,					\
	.fsid = -1,					\
	.fileid = -1,					\
	.size = -1,					\
	.bsize = -1,					\
	.atim = { .tv_sec = -1, .tv_nsec = -1 },	\
	.mtim = { .tv_sec = -1, .tv_nsec = -1 },	\
	.ctim = { .tv_sec = -1, .tv_nsec = -1 },	\
	.rdev = -1,					\
	.dsize = -1,					\
};

/*
 * A virtual node
 */
typedef struct vnode {
	atomic_uint	refcount;
	struct vfs	*vfs;
	vtype_t		type;
	struct vnode_ops *ops;
	TAILQ_ENTRY(vnode) vfs_vnlistentry; /* entry in vfs::vnode_list */
	bool		published;	/* on vfs::vnode_list */

	uint32_t	release_pending;
	TAILQ_ENTRY(vnode) release_link;

	kspinlock_t	nc_lock;
	uint32_t	nc_refs_n;	/* (nc_lock) */
	LIST_HEAD(, namecache) nc_refs; /* (nc_lock) */

	union {
		struct {
			struct vn_vc_state	*vc_state;
			struct vm_object	*vmobj;
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

struct vnode_ops {
	size_t stack_depth;
	int (*open)(vnode_t **, int);
	int (*close)(vnode_t *, int);
	int (*create)(vnode_t *, const char *, vattr_t *, vnode_t **);
	int (*link)(vnode_t *, vnode_t *, const char *);
	int (*remove)(vnode_t *dvn, const char *name, vnode_t *vn);
	int (*rmdir)(vnode_t *dvn, const char *name, vnode_t *vn);
	int (*rename)(vnode_t *, const char *, vnode_t *,
	    const char *);
	int (*ioctl)(vnode_t *, unsigned long cmd, void *data);
	int (*getattr)(vnode_t *, vattr_t*);
	int (*setattr)(vnode_t *, vattr_t*);
	int (*lookup)(vnode_t *, const char *, vnode_t **);
	int (*readdir)(vnode_t *, void *buf, size_t length, off_t *offset);
	int (*readlink)(vnode_t *, char *buf, size_t buflen);
	int (*inactive)(vnode_t *);
	int (*fsync)(vnode_t *, int flags);
	int (*read)(vnode_t *, void *buf, size_t length, off_t offset,
	    int flags);
	int (*write)(vnode_t *, const void *buf, size_t length, off_t offset,
	    int flags);
	int (*seek)(vnode_t *, off_t old_offset, off_t *new_offset);
	int (*chpoll)(vnode_t *, struct poll_entry *, enum chpoll_mode);
	int (*mmap)(void *addr, size_t len, int prot, int flags, vnode_t *vn,
	    off_t offset, vaddr_t *window);
	void (*vc_enter)(vnode_t *, bool write);
	void (*vc_exit)(vnode_t *, bool write);
	iop_return_t (*iop_dispatch)(vnode_t *, struct iop *);
	iop_return_t (*iop_complete)(vnode_t *, struct iop *);
	void (*paging_enter)(vnode_t *);
	void (*paging_exit)(vnode_t *);
};

/* for vnode_ops::fsync */
#define VN_FSYNC_DURABLE	0x1	/* fsync(2) semantics*/
#define VN_FSYNC_DATAONLY	0x2	/* fdatasync(2) semantics */

#define VOP_OPEN(VN, FLAGS) (*(VN))->ops->open(VN, FLAGS);
#define VOP_FSYNC(VN, FLAGS) (VN)->ops->fsync(VN, (FLAGS))
#define VOP_CLOSE(VN, FLAGS) (VN)->ops->close(VN, FLAGS)
#define VOP_RENAME(SVN, SNAME, DVN, DNAME) \
    (SVN)->ops->rename(SVN, SNAME, DVN, DNAME)
#define VOP_CHPOLL(VN, POLL, MODE) (VN)->ops->chpoll(VN, (POLL), (MODE));
#define VOP_READLINK(VN, BUF, LEN) (VN)->ops->readlink(VN, (BUF), (LEN));
#define VOP_GETATTR(VN, VATTR) (VN)->ops->getattr(VN, (VATTR));
#define VOP_SETATTR(VN, VATTR) (VN)->ops->setattr(VN, (VATTR));
#define VOP_IOCTL(VN, CMD, DATA) (VN)->ops->ioctl(VN, (CMD), (DATA));
#define VOP_READ(VN, BUF, LEN, OFF, F) (VN)->ops->read(VN, BUF, LEN, OFF, F);
#define VOP_READDIR(VN, BUF, LEN, OFF) (VN)->ops->readdir(VN, BUF, LEN, OFF);
#define VOP_WRITE(VN, BUF, LEN, OFF, F) (VN)->ops->write(VN, BUF, LEN, OFF, F);
#define VOP_SEEK(VN, OLD, NEW) (VN)->ops->seek(VN, (OLD), (NEW));
#define VOP_VC_ENTER(VN, WRITE) (VN)->ops->vc_enter(VN, (WRITE));
#define VOP_VC_EXIT(VN, WRITE) (VN)->ops->vc_exit(VN, (WRITE));
#define VOP_PAGING_ENTER(VN) (VN)->ops->paging_enter(VN)
#define VOP_PAGING_EXIT(VN) (VN)->ops->paging_exit(VN)

void vnode_init(void);

vnode_t *vn_alloc(struct vfs *, vtype_t, struct vnode_ops *, uintptr_t fspriv1, uintptr_t fspriv2);
/*!
 * @brief Publish a vnode into it's VFS' vnode list.
 */
void vn_publish(vnode_t *);

/*!
 * @brief Free a frozen vnode.
 *
 * Its viewcache state and VM object must be empty already. If it was published,
 * the VFS waitq is woken after this.
 */
void vn_free(vnode_t *);

/*!
 * @brief Retain a reference on a vnode (must already have a reference.)
 */
vnode_t *vn_retain(vnode_t *);

/*!
 * @brief Try to retain a reference on a vnode. Fails if it's frozen.
 *
 * @retval true vnode retained.
 * @retval false vnode was frozen, can't be retained.
 */
bool vn_tryretain(vnode_t *);

/*!
 * @brief Try to freeze a vnode given caller holds the last retain.
 *
 * @retval true vnode frozen
 * @retval false vnode not frozen as other references remain.
 */
bool vn_tryfreeze(vnode_t *);

/*!
 * @brief Release a reference on a vnode, potentially tearing it down.
 */
void vn_release(vnode_t *);

/*!
 * @brief Queue (from an unsuitable context) a vnode for deferred release.
 */
void vn_release_async(vnode_t *);

int viewcache_io(vnode_t *, uint64_t offset, size_t length, bool write,
    void *buf);
void viewcache_truncate(vnode_t *, uint64_t newsize);
/*!
 * @brief Write back every dirty view of a vnode, waiting for any in writeback.
 */
void viewcache_sync_vnode(vnode_t *);
struct vn_vc_state *viewcache_alloc_vnode_state(vnode_t *vn);
void viewcache_free_vnode_state(vnode_t *vn);

#endif /* ECX_SYS_VNODE_H */
