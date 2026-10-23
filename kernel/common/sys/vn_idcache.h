/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vn_idcache.h
 * @brief Generic FS identifier-to-vnode cache.
 *
 * Library facilities for filesystems which have stable identifiers (say, inode
 * numbers, 9p QIDs, NFS handles, etc) for their nodes.
 *
 * Such filesystems can embed a vn_idcache_t in their per-VFS state, and a
 * vn_idcache_entry_t in their nodes. The routines here then provide the map
 * from identifier keys to vnodes, prevents publishing duplicate vnodes, and
 * serialises lookup against teardown, as when a vnode is being torn down but
 * someone looks it up again. (This provides a wait for the old vnode to first
 * be torn down.)
 *
 * TO THINK ABOUT - should we generalise and allow not-yet-constructed vnodes to
 * live in the cache too, and also get the waiting treatment for lookups? that
 * eliminates the problem of allocating a fully-set-up vnode and then
 * potentially making away with it if there is already a vnode in the cache -
 * not a real harm for 9pFS but could this be problematic for future FSes?
 *
 * bit that may invite questions - we already block for a frozen vnode to go
 * away while potentially at another level (the FS, say) a lock is held! With
 * construction, another place it can happen - needs thinking about.
 */

#ifndef ECX_SYS_VN_IDCACHE_H
#define ECX_SYS_VN_IDCACHE_H

#include <sys/k_thread.h>
#include <sys/k_waitq.h>

#include <libkern/queue.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct vnode;

/*!
 * Per-mounted filesystem ID cache state.
 */
typedef struct vn_idcache {
	kmutex_t lock;
	size_t nbuckets;
	LIST_HEAD(vn_idcache_bucket, vn_idcache_entry) *buckets;
	size_t count;
	kwaitq_t waitq;	/*!< waits for vnodes being torn down to go away */
} vn_idcache_t;

/*!
 * Per-FS node ID cache state.
 */
typedef struct vn_idcache_entry {
	LIST_ENTRY(vn_idcache_entry) link;
	struct vnode *vn;
	const void *key;
	size_t keylen;
	uint32_t hash;
	bool inserted;
} vn_idcache_entry_t;

/*!
 * Decision made on disposal of a vnode.
 */
enum vn_idcache_decision {
	/*!
	 * The vnode should be frozen and teardown proceed.
	 */
	kVcacheInactiveProceed,
	/*!
	 * The vnode should be retained, e.g. for posting it to someone to do
	 * writeback, who should then release the vnode.
	 */
	kVcacheInactiveRetain,
	/*!
	 * The inactivation should be cancelled, i.e. reference count was no
	 * longer 1.
	 */
	kVcacheInactiveEAGAIN,
};

/*!
 * Callback invoked with the ID cache locked for the filesystem to decide on
 * disposal of the vnode. Must not block.
 */
typedef enum vn_idcache_decision (*vn_idcache_decide_fn)(struct vnode *, void *);

/*!
 * @brief Initialise an ID cache with at least \p nbuckets buckets.
 */
void vn_idcache_init(vn_idcache_t *, size_t nbuckets);

/*!
 * @brief Destroy an empty cache.
 */
void vn_idcache_fini(vn_idcache_t *);

/*!
 * @brief Initialise an ID cache entry for \p vn.
 *
 * The key bytes in \p key are not copied, they must remain immutable.
 */
void vn_idcache_entry_init(vn_idcache_entry_t *, struct vnode *vn,
    const void *key, size_t keylen);

/*!
 * @brief Look up the vnode for a given key.
 *
 * If a vnode is found for the key but is frozen then this blocks until it goes.
 *
 * @retval 0 vnode found and a retained reference to it was written to \p *out.
 * @retval -ENOENT no vnode exists for this key.
 */
int vn_idcache_find(vn_idcache_t *, const void *key, size_t keylen,
    struct vnode **out);

/*!
 * @brief Insert a vnode into the cache for a given key, or if an existing vnode
 * already exists for that key in the cache, get a retained reference to it.
 *
 * Does NOT consume a retain. If a frozen vnode is found then this waits for it.
 *
 * @retval 0 vnode inserted and published to the VFS' vnode list.
 * @retval -EEXIST vnode already exists for the key, caller should free his node
 */
int vn_idcache_insert(vn_idcache_t *, vn_idcache_entry_t *, struct vnode **out);

/*!
 * @brief Unlink an entry if it is linked.
 */
void vn_idcache_remove(vn_idcache_t *, vn_idcache_entry_t *);

/*!
 * @brief Change an entry's key.
 *
 * The \p newkey must be new storage. If there is a frozen vnode under that
 * identity then this function waits for it to go away.
 *
 * @retval 0 success
 * @retval -EEXIST another vnode exists for \p newkey - NO change made.
 */
int vn_idcache_rekey(vn_idcache_t *, vn_idcache_entry_t *, const void *newkey,
    size_t newlen);

/*!
 * @brief Possibly begin inactivation of a vnode, if neither \p decide nor the
 * cache itself veto it.
 *
 * To be called from a filesystem's inactive op. The ID cache can veto teardown
 * (e.g. if someone else raced to lookup the vnode); this routine locks the
 * cache so no one else can race and then calls \p decide so the filesystem can
 * decide whether it is happy to proceed with teardown.
 *
 * Anyone looking up the key after this returns with #kVcacheInactiveProceed
 * will block until vn_idcache_inactive_end() is called.
 *
 * @returns the committed decision
 */
enum vn_idcache_decision vn_idcache_inactive_begin(vn_idcache_t *,
    vn_idcache_entry_t *, vn_idcache_decide_fn decide, void *arg);

/*!
 * @brief Complete teardown of a vnode.
 *
 * To be called when it is safe for others to instantiate a vnode for the same
 * unique identifier, i.e. generally after all dirty state is written back, no
 * dirty pages/views remain, etc. As after this is called anyone looking up the
 * key will no longer find the vnode that used to be registered thereunder.
 */
void vn_idcache_inactive_end(vn_idcache_t *, vn_idcache_entry_t *);

#endif /* ECX_SYS_VN_IDCACHE_H */
