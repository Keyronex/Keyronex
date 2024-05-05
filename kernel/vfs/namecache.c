#include <errno.h>

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"

static int64_t nc_cmp(struct namecache *x, struct namecache *y);
static void nc_trim_lru(void);
RB_GENERATE(namecache_rb, namecache, rb_entry, nc_cmp);

static size_t n_inactive = 0;
static size_t max_inactive = 256;
static kmutex_t lru_mutex = KMUTEX_INITIALIZER(lru_mutex);
static struct namecache_tailq lru_queue = TAILQ_HEAD_INITIALIZER(lru_queue);
namecache_handle_t root_nch;

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
nc_hash(const char *str, size_t len)
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
	obj_release(ncp->vp);
	ncp->vp = NULL;
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
		TAILQ_INSERT_TAIL(&lru_queue, ncp, lru_entry);
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

	ncp = TAILQ_FIRST(&lru_queue);
	TAILQ_REMOVE(&lru_queue, ncp, lru_entry);

	/*
	 * Assumptions:
	 *
	 * Since we are pulling the NCP off the LRU queue, we are responsible
	 * for its destruction.
	 *
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

int
nc_lookup(struct namecache *nc, struct namecache **out, const char *name)
{
	struct namecache *found, key;
	int r;

	ke_wait(&nc->mutex, "nc_lookup:nc->muetx", false, false, -1);

	key.name = (char *)name;
	key.name_len = strlen(name);
	key.key = nc_hash(name, key.name_len);

	found = RB_FIND(namecache_rb, &nc->entries, &key);

	if (found == NULL) {
		vnode_t *vnode;
		found = kmem_alloc(sizeof(namecache_t));

		ke_mutex_init(&found->mutex);
		found->name = strdup(name);
		found->name_len = key.name_len;
		found->key = key.key;
		found->parent = nc_retain(nc);
		RB_INIT(&found->entries);
		RB_INSERT(namecache_rb, &nc->entries, found);

		r = nc->vp->ops->lookup(nc->vp, &vnode, name);
		if (r != 0) {
			/* make a negative entry */
			found->refcnt = 0;
			found->vp = NULL;
			TAILQ_INSERT_TAIL(&lru_queue, found, lru_entry);
			n_inactive++;
			ke_mutex_release(&nc->mutex);
			return -ENOENT;
		} else {
			found->refcnt = 1;
			found->vp = vnode;
		}
	} else if (found->vp == NULL) {
		/* negative entry */
		ke_mutex_release(&nc->mutex);
		return -ENOENT;
	}

	nc_retain(found);
	ke_mutex_release(&nc->mutex);

	return 0;
}

void
nc_make_root(vfs_t *vfs, vnode_t *vnode)
{
	namecache_t *ncp = kmem_alloc(sizeof(namecache_t));

	ke_mutex_init(&ncp->mutex);
	ncp->name = NULL;
	ncp->name_len = 0;
	ncp->key = 0;
	ncp->refcnt = 1;
	RB_INIT(&ncp->entries);
	RB_INSERT(namecache_rb, &ncp->entries, ncp);
	ncp->vp = vnode;

	root_nch.nc = ncp;
	root_nch.vfs = vfs;
}
