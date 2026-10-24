/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file vfs.h
 * @brief VFS declarations.
 */

#ifndef ECX_SYS_KRX_VFS_H
#define ECX_SYS_KRX_VFS_H

#include <sys/types.h>
#include <sys/k_intr.h>
#include <sys/k_waitq.h>
#include <sys/krx_atomic.h>
#include <sys/tree.h>

#include <libkern/queue.h>

/*!
 * A namecache entry
 *
 * In flags, NC_ACCESSED can be set by any retainer but the rest require
 * the topology lock held write.
 */
typedef struct namecache {
	atomic_uint refcnt;
	atomic_uint flags;	/* NC_* flags and mount count */
	struct vnode *vp;
	LIST_ENTRY(namecache) vn_link; /* on vp->nc_refs */
	TAILQ_ENTRY(namecache) lru_qlink; /* (lru_mutex) */
	struct namecache *parent;
	RB_ENTRY(namecache) sib_rblink;
	RB_HEAD(namecache_rb, namecache) children;
	char *name;
	uint64_t key; /*!< rb key: len(name) << 32 | hash(name) */

	/*
	 * TODO: maybe union this with something to save space?
	 * since we retain the namecache across operations, maybe standby link?
	 */
	kwaitq_t busy_waitq;
} namecache_t;

#define NC_BUSY (1u << 0)	/*!< busy resolving, wait on busy_waitq */
#define NC_ON_LRU (1u << 1)	/*!< on (and retained by) an LRU queue */
#define NC_ACCESSED (1u << 2)	/*!< accessed bit for replacement */
#define NC_DISSOCIATED (1u << 3) /*!< removed from hierarchy  */
#define NC_MOUNTS_SHIFT 8	/*!< number of vfses mounted over this entry */
#define NC_MOUNTS_MASK (0xffu << NC_MOUNTS_SHIFT)

typedef struct namecache_handle {
	namecache_t *nc;
	struct vfs *vfs;
} namecache_handle_t;

#define NCH_NULL ((namecache_handle_t) { NULL, NULL })

struct vfs_ops;

/*
 * A virtual file system
 *
 * v: vnode_list_lock
 */
typedef struct vfs {
	atomic_uint opencnt; /*!< bits 31..1 for count; bit 0 for umounting */
	const struct vfs_ops *ops;

	namecache_t *root_nc;
	namecache_handle_t nchcovered;
	LIST_ENTRY(vfs) mountpoint_hash_entry;
	TAILQ_ENTRY(vfs) mount_link; /* on mount list (vfs_mount_lock) */

	kspinlock_t vnode_list_lock;
	TAILQ_HEAD(, vnode) vnode_list; /* (v) vnodes */
	kwaitq_t vnode_waitq; /* woken on vn_free of a published vnode */

	uintptr_t fsprivate_1;
} vfs_t;

/*!
 * Virtual filesystem operations
 */
struct vfs_ops {
	/*!
	 * @brief Prepare root vnode for inactivation.
	 *
	 * Called during unmount after every vnode except root has gone. The FS
	 * can clean up anything needful before the root namecache entry is
	 * released and the root vnode's inactive routine is thus called.
	 */
	void (*unmount)(vfs_t *);
	/*!
	 * @brief Free filesystem state.
	 *
	 * By now all vnodes of the VFS are gone, so the FS can do any remaining
	 * cleanup and finally free the VFS itself.
	 */
	void (*freevfs)(vfs_t *);
};

enum lookup_flags {
	LOOKUP_NOFOLLOW_FINAL = 1 << 0,
	LOOKUP_2NDLAST = 1 << 1,
	LOOKUP_CREATE = 1 << 2,
	LOOKUP_ALLOW_NEG = 1 << 3,
	/*!
	 * When paired with LOOKUP_CREATE, requires the final name not already
	 * exist, and EEXIST is returned if it does.
	 * LOOKUP_NOFOLLOW_FINAL is implied by this flag.
	 */
	LOOKUP_EXCL = 1 << 4,
};

struct lookup_info {
	namecache_handle_t start; /* starting point for lookup */
	const char *path;	  /* path to look up relative to start */
	enum lookup_flags flags;  /* flags */
	struct vattr *create_attr;	  /* attributes for LOOKUP_CREATE */

	namecache_handle_t result; /* result of the lookup */
	bool did_create;	   /* whether a node was created */
};


void nc_makeroot(vfs_t *vfs, struct vnode *root_vn);

/*!
 * @brief Mount vfs (with root vnode rootvn) over directory overnch.
 *
 * Consumes \p overnch
 */
void nc_domount(namecache_handle_t overnch, vfs_t *vfs, struct vnode *rootvn);

int nc_link(namecache_handle_t dirnch, struct vnode *target_vn,
    const char *name);

int nc_remove(namecache_handle_t dirnch, const char *name, bool isdir);

int nc_rename(namecache_handle_t old_dirnch, const char *old_name,
    namecache_handle_t new_dirnch, const char *new_name);

/*!
 * @brief Try to find a live name of a vnode from its namecache links.
 *
 * If successful then a retained vnode to the directory holding the name is
 * written to \p *parent_out , and the name to \p name.
 *
 * @retval 0 successful
 * @retval -ENOENT no live name for vnode
 */
int nc_vnode_live_name(struct vnode *vn, struct vnode **parent_out, char *name,
    size_t namelen);

int vfs_lookup_init(struct lookup_info *info, namecache_handle_t start,
    const char *path, enum lookup_flags flags);
int vfs_lookup(struct lookup_info *info);
int vfs_lookup_simple(namecache_handle_t start, namecache_handle_t *out,
    const char *path, enum lookup_flags flags);

void vfs_init(vfs_t *);

/*!
 * @brief Try to retain a VFS.
 *
 * @retval 0 success
 * @retval -EBUSY the VFS is being unmounted.
 */
int vfs_tryretain(vfs_t *vfs);

/*!
 * @brief Release a reference to a VFS.
 */
void vfs_release(vfs_t *vfs);

int vfs_sync(struct vfs *, int fsync_flags);
int vfs_sync_all(int fsync_flags);

/*!
 * @brief Unmount the VFS with root \p nch .
 *
 * @retval 0 unmounted successfully.
 * @retval -EBUSY others reference the VFS, or it is root (not unmountable).
 * @retval -EINVAL \p nch is not the root nch of a VFS.
 */
int vfs_unmount(namecache_handle_t nch);

/*!
 * @brief Wait for a given VFS to have at most \p remaining vnodes.
 *
 * i.e. for unmount.
 * TODO: this currently waits for at most 30s and then panics.
 */
void vfs_wait_vnodes(struct vfs *, size_t remaining);

namecache_handle_t nchandle_retain(namecache_handle_t in);
namecache_handle_t nchandle_release(namecache_handle_t in);

int sys_openat(int dirfd, const char *upath, int flags, mode_t mode);
int sys_faccessat(int dirfd, const char *path, int mode, int flags);
int sys_mkdirat(int dirfd, const char *path, mode_t mode);
int sys_linkat(int olddirfd, const char *oldpath, int newdirfd,
    const char *newpath, int flags);
int sys_unlinkat(int dirfd, const char *upath, int flags);
int sys_renameat(int olddirfd, const char *oldpath, int newdirfd,
    const char *newpath);
int sys_readlinkat(int dirfd, const char *upath, char *ubuf, size_t bufsiz);
int sys_truncate(const char *upath, off_t length);
int sys_fchmodat(int dirfd, const char *pathname, mode_t mode, int flags);
int sys_fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group,
    int flags);

int sys_sync(void);
int sys_mount(const char *usource, const char *utarget, const char *ufstype,
    unsigned long flags, const void *udata);
int sys_umount(const char *utarget, int flags);

extern namecache_handle_t root_nch;

#endif /* ECX_SYS_KRX_VFS_H */
