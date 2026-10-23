/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file vn_idcache.c
 * @brief Generic FS identity cache.
 *
 * entry lock -> idcache mutex -> idcache waitq spinlock.
 */

#include <sys/errno.h>
#include <sys/k_log.h>
#include <sys/kmem.h>
#include <sys/vn_idcache.h>
#include <sys/vnode.h>

#include <libkern/lib.h>

#include <stdatomic.h>

static inline struct vn_idcache_bucket *
bucket_for(vn_idcache_t *cache, uint32_t hash)
{
	return &cache->buckets[hash & (cache->nbuckets - 1)];
}

/* if skip is set, then that entry is skipped (i.e. for rekeying)  */
static vn_idcache_entry_t *
lookup_locked(vn_idcache_t *cache, const void *key, size_t keylen,
    uint32_t hash, vn_idcache_entry_t *skip)
{
	vn_idcache_entry_t *e;

	LIST_FOREACH (e, bucket_for(cache, hash), link) {
		if (e != skip && e->hash == hash && e->keylen == keylen &&
		    memcmp(e->key, key, keylen) == 0)
			return e;
	}

	return NULL;
}

static void
link_locked(vn_idcache_t *cache, vn_idcache_entry_t *entry)
{
	kassert(ke_mutex_held(&cache->lock));

	kassert(!entry->inserted, "vn_idcache: entry already linked");
	LIST_INSERT_HEAD(bucket_for(cache, entry->hash), entry, link);
	entry->inserted = true;
	cache->count++;
}

/* returns false if the entry was never inserted. */
static bool
unlink_locked(vn_idcache_t *cache, vn_idcache_entry_t *entry)
{
	kassert(ke_mutex_held(&cache->lock));

	if (!entry->inserted)
		return false;

	LIST_REMOVE(entry, link);
	entry->inserted = false;
	kassert(cache->count > 0, "vn_idcache: count underflow");
	cache->count--;

	return true;
}

/* releases cache->lock, which must be held at entry */
static void
wait_frozen(vn_idcache_t *cache, const char *reason)
{
	kwaiter_t waiter;

	ke_waitq_prepare(&cache->waitq, &waiter);
	ke_mutex_exit(&cache->lock);
	ke_waitq_wait(&cache->waitq, &waiter, reason, false, ABSTIME_FOREVER);
}

void
vn_idcache_init(vn_idcache_t *cache, size_t nbuckets)
{
	size_t n = 1;

	while (n < nbuckets)
		n <<= 1;

	ke_mutex_init(&cache->lock);
	ke_waitq_init(&cache->waitq);
	cache->nbuckets = n;
	cache->count = 0;
	cache->buckets = kmem_alloc(n * sizeof(*cache->buckets));
	for (size_t i = 0; i < n; i++)
		LIST_INIT(&cache->buckets[i]);
}

void
vn_idcache_fini(vn_idcache_t *cache)
{
	kassert(cache->count == 0, "vn_idcache_fini: cache still has entries");
	kmem_free(cache->buckets, cache->nbuckets * sizeof(*cache->buckets));
	cache->buckets = NULL;
	cache->nbuckets = 0;
}

void
vn_idcache_entry_init(vn_idcache_entry_t *entry, vnode_t *vn, const void *key,
    size_t keylen)
{
	entry->vn = vn;
	entry->key = key;
	entry->keylen = keylen;
	entry->hash = fnv1a_hash32(key, keylen);
	entry->inserted = false;
}

int
vn_idcache_find(vn_idcache_t *cache, const void *key, size_t keylen,
    vnode_t **out)
{
	uint32_t hash = fnv1a_hash32(key, keylen);

	for (;;) {
		vn_idcache_entry_t *e;

		ke_mutex_enter(&cache->lock, "vn_idcache_find");

		e = lookup_locked(cache, key, keylen, hash, NULL);
		if (e == NULL) {
			ke_mutex_exit(&cache->lock);
			return -ENOENT;
		}

		if (vn_tryretain(e->vn)) {
			*out = e->vn;
			ke_mutex_exit(&cache->lock);
			return 0;
		}

		wait_frozen(cache, "vn_idcache_find");
	}
}

int
vn_idcache_insert(vn_idcache_t *cache, vn_idcache_entry_t *entry, vnode_t **out)
{
	kassert(!entry->inserted, "vn_idcache_insert: vnode already inserted");
	kassert(atomic_load_explicit(&entry->vn->refcount,
	    memory_order_relaxed) != 0,
	    "vn_idcache_insert: vnode refcount not 0 as it should be");

	for (;;) {
		vn_idcache_entry_t *e;

		ke_mutex_enter(&cache->lock, "vn_idcache_insert");

		e = lookup_locked(cache, entry->key, entry->keylen,
		    entry->hash, NULL);
		if (e == NULL) {
			link_locked(cache, entry);
			vn_publish(entry->vn);
			ke_mutex_exit(&cache->lock);
			return 0;
		}

		if (vn_tryretain(e->vn)) {
			*out = e->vn;
			ke_mutex_exit(&cache->lock);
			return -EEXIST;
		}

		wait_frozen(cache, "vn_idcache_insert");
	}
}

void
vn_idcache_remove(vn_idcache_t *cache, vn_idcache_entry_t *entry)
{
	ke_mutex_enter(&cache->lock, "vn_idcache_remove");
	(void)unlink_locked(cache, entry);
	ke_mutex_exit(&cache->lock);
}

int
vn_idcache_rekey(vn_idcache_t *cache, vn_idcache_entry_t *entry,
    const void *newkey, size_t newlen)
{
	uint32_t newhash = fnv1a_hash32(newkey, newlen);

	for (;;) {
		vn_idcache_entry_t *e;
		bool was_linked;

		ke_mutex_enter(&cache->lock, "vn_idcache_rekey");

		e = lookup_locked(cache, newkey, newlen, newhash, entry);
		if (e != NULL) {
			if (atomic_load_explicit(&e->vn->refcount,
				memory_order_relaxed) != 0) {
				ke_mutex_exit(&cache->lock);
				return -EEXIST;
			}

			wait_frozen(cache, "vn_idcache_rekey");
			continue;
		}

		was_linked = unlink_locked(cache, entry);
		entry->key = newkey;
		entry->keylen = newlen;
		entry->hash = newhash;
		if (was_linked)
			link_locked(cache, entry);

		ke_mutex_exit(&cache->lock);

		return 0;
	}
}

enum vn_idcache_decision
vn_idcache_inactive_begin(vn_idcache_t *cache, vn_idcache_entry_t *entry,
    vn_idcache_decide_fn decide, void *arg)
{
	vnode_t *vn = entry->vn;
	enum vn_idcache_decision d;

	ke_mutex_enter(&cache->lock, "vn_idcache_inactive_begin");

	d = decide(vn, arg);
	switch (d) {
	case kVcacheInactiveProceed:
		if (!vn_tryfreeze(vn))
			d = kVcacheInactiveEAGAIN; /* someone got to it first*/
		break;

	case kVcacheInactiveRetain:
		vn_retain(vn); /* fine, refcnt still >= 1 at this point*/
		break;

	case kVcacheInactiveEAGAIN:
		break;

	default:
		kfatal("vn_idcache_inactive_begin: bad decision %d\n", d);
	}

	ke_mutex_exit(&cache->lock);

	return d;
}

void
vn_idcache_inactive_end(vn_idcache_t *cache, vn_idcache_entry_t *entry)
{
	kassert(atomic_load_explicit(&entry->vn->refcount,
		    memory_order_relaxed) == 0,
	    "vn_idcache_inactive_end: vnode not frozen");

	ke_mutex_enter(&cache->lock, "vn_idcache_inactive_end");
	(void)unlink_locked(cache, entry);
	ke_mutex_exit(&cache->lock);

	ke_waitq_wake_all(&cache->waitq);
}
