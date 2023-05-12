/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri May 12 2023.
 */

#include <kdk/kernel.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/namecache.h>
#include <kdk/object.h>

TAILQ_HEAD(namecache_tailq, namecache);

static int64_t nc_cmp(struct namecache *x, struct namecache *y);
static void nc_trim_lru(void);
RB_GENERATE(namecache_rb, namecache, rb_entry, nc_cmp);

static size_t n_inactive = 0;
static size_t max_inactive = 256;
static kmutex_t lru_mutex = KMUTEX_INITIALIZER(lru_mutex);
static struct namecache_tailq lru_queue = TAILQ_HEAD_INITIALIZER(lru_queue);

static int64_t
nc_cmp(struct namecache *x, struct namecache *y)
{
	if (x->key < y->key)
		return -1;
	else if (x->key > y->key)
		return 1;
	kassert(x->name_len == y->name_len);
	return memcmp(x->name, y->name, x->name_len);
}

static uint64_t
nc_hash(char *str, size_t len)
{
	static const uint32_t fnv_prime = 0x811C9DC5, fnv_basis = 0x01000193;
	uint32_t hash = fnv_basis;

	for (size_t i = 0; i < len; i++) {
		hash *= fnv_prime;
		hash ^= str[i];
	}

	return hash | ((uint64_t)len << 32);
}

static void *
nc_free(struct namecache *ncp)
{
	kassert(ncp->mutex.owner == NULL);
	kassert(RB_EMPTY(&ncp->entries));
	ncp->vp = obj_direct_release(ncp->vp);
	kmem_strfree(ncp->name);
	kmem_free(ncp, sizeof(*ncp));
	return NULL;
}

/*
 * If nested is true:
 *  - entered with ncp->mutex and lru_mutex held
 *  - exits with ncp->mutex NOT held.
 * Otherwise:
 *  - entered with ncp->mutex and lru_mutex NOT held
 *  - exits with both NOT held.
 *
 */
struct namecache *
nc_release_internal(struct namecache *ncp, bool nested)
{
	if (__atomic_fetch_sub(&ncp->refcnt, 1, __ATOMIC_RELEASE) == 1) {
		/* refcnt at zero - free (if orphaned) or emplace on LRU */
		if (ncp->parent == NULL) {
			if (nested)
				ke_mutex_release(&ncp->mutex);
			return nc_free(ncp);
		}

		if (!nested) {
			ke_wait(&ncp->mutex, "nc_release:nc->mutex", false,
			    false, -1);
			ke_wait(&lru_mutex, "nc_retain:lru_mutex", false, false,
			    -1);
		}
		kassert(ncp->refcnt == 0);
		TAILQ_INSERT_HEAD(&lru_queue, ncp, lru_entry);
		n_inactive++;
		if (!nested) {
			ke_mutex_release(&ncp->mutex);
			if (n_inactive > max_inactive)
				nc_trim_lru();
			ke_mutex_release(&lru_mutex);
		}
	} else if (nested) {
		ke_mutex_release(&ncp->mutex);
	}
	return NULL;
}

static void
nc_trim_lru(void)
{
	struct namecache *ncp, *parent;

loop:
	if (n_inactive <= max_inactive)
		return;

	ncp = TAILQ_LAST(&lru_queue, namecache_tailq);

	/*
	 * Assumptions:
	 * If there is a parent pointer, then parent cannot be blocked on the
	 * lru_mutex, because parent's refcnt must be > 0.
	 *
	 * I think we are safe cross-thread, but intuition warns me we might
	 * need recursive mutexes.
	 *
	 * Things will also get a little more complicated if vnodes acquire a
	 * path to namecaches.
	 */

	parent = ncp->parent;
	ke_wait(&parent->mutex, "nc_trim_lru:parent->mutex", false, false, -1);
	ke_wait(&ncp->mutex, "nc_trim_lru:ncp->mutex", false, false, -1);

	kassert(ncp->parent == parent);
	kassert(ncp->refcnt == 0);
	RB_REMOVE(namecache_rb, &ncp->parent->entries, ncp);
	nc_free(ncp);
	nc_release_internal(ncp->parent, true);

	goto loop;
}

struct namecache *
nc_retain(struct namecache *ncp)
{
	if (__atomic_fetch_add(&ncp->refcnt, 1, __ATOMIC_RELEASE) == 0) {
		/* first reference after dropping to zero - remove from LRU */
		ke_mutex_assert_held(&ncp->mutex);
		/* orphaned ncs should be immediately freed when refcnt = 0 */
		kassert(ncp->parent != NULL);
		ke_wait(&lru_mutex, "nc_retain:lru_mutex", false, false, -1);
		TAILQ_REMOVE(&lru_queue, ncp, lru_entry);
		ke_mutex_release(&lru_mutex);
	}
	return ncp;
}

struct namecache *
nc_release(struct namecache *ncp)
{
	return nc_release_internal(ncp, false);
}
