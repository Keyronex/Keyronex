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
#include <sys/krx_atomic.h>
#include <sys/tree.h>

#include <libkern/queue.h>

typedef struct namecache {
	atomic_uint refcnt;
	uint32_t mounts_over_n : 8, busy: 1, unused : 23;
	struct vnode *vp;
	TAILQ_ENTRY(namecache) standby_qlink;
	struct namecache *parent;
	RB_ENTRY(namecache) sib_rblink;
	RB_HEAD(namecache_rb, namecache) children;
	char *name;
	uint64_t key; /*!< rb key: len(name) << 32 | hash(name) */

	/*
	 * TODO: maybe union these with something to save space?
	 * since we retain the namecache across operations, maybe standby link?
	 */
	kspinlock_t waiters_lock;
	LIST_HEAD(,namecache_waiter) waiters;
} namecache_t;

typedef struct namecache_handle {
	namecache_t *nc;
	struct vfs *vfs;
} namecache_handle_t;

#define NCH_NULL ((namecache_handle_t) { NULL, NULL })

typedef struct vfs {
	atomic_uint opencnt; /*!< bits 31..1 for count; bit 0 for umounting */

	namecache_t *root_nc;
	namecache_handle_t nchcovered;
	LIST_ENTRY(vfs) mountpoint_hash_entry;

	kspinlock_t vnode_list_lock;
	TAILQ_HEAD(, vnode) vnode_list;

	uintptr_t fsprivate_1;
} vfs_t;

enum lookup_flags {
	LOOKUP_NOFOLLOW_FINAL = 1 << 0,
	LOOKUP_2NDLAST = 1 << 1,
	LOOKUP_CREATE = 1 << 2,
	LOOKUP_ALLOW_NEG = 1 << 3,
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
int nc_link(namecache_handle_t dirnch, struct vnode *target_vn,
    const char *name);
int nc_remove(namecache_handle_t dirnch, const char *name, bool isdir);
int nc_rename(namecache_handle_t old_dirnch, const char *old_name,
    namecache_handle_t new_dirnch, const char *new_name);

int vfs_lookup_init(struct lookup_info *info, namecache_handle_t start,
    const char *path, enum lookup_flags flags);
int vfs_lookup(struct lookup_info *info);
int vfs_lookup_simple(namecache_handle_t start, namecache_handle_t *out,
    const char *path, enum lookup_flags flags);

void vfs_init(vfs_t *);

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

namecache_handle_t nchandle_retain(namecache_handle_t in);
namecache_handle_t nchandle_release(namecache_handle_t in);

extern namecache_handle_t root_nch;

#endif /* ECX_SYS_KRX_VFS_H */
