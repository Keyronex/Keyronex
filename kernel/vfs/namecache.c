#include <errno.h>

#include "kdk/kmem.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/vfs.h"

/*!
 * These are some initial notes on namecaches. Some of this should go in a big
 * theory statement instead, and needs to be written about in the Keyronex book.
 *
 * A namecache represents an element in the filesystem namespace - a file,
 * folder, device, etc. They hold a pointer to a parent namecache and an RB tree
 * of child namecaches.
 *
 * Namecaches are reference-counted. While there is a reference count greater
 * than 1, they may not be freed. When reference count is at 0, the namecache
 * is entered onto an LRU list; it may be dropped when it reaches the end of
 * that list.
 *
 * The pointer to the parent is a retaining pointer; the RB tree of children are
 * weak pointers.
 *
 * A general parent-before-child lock ordering exists along with a pointer
 * comparison (lower first) ordering for the acquisition of 'sibling' locks.
 *
 * Rough notes below to be structured:
 *
 * namecaches retain their vnodes.
 *
 * the vnode pointer may currently only transition once - from NULL to non-NULL.
 * thereafter it is as durable as the namecache itself.
 *
 * name and key may change (e.g. with rename) ofc under tight locking
 * constraints, i.e. parent locked and nc locked (? should we create new
 * namecaches instead? does it matter?)
 */

struct ncstat {
	size_t inactive;
	size_t max_inactive;
} ncstat = { 0, 256 };

static int64_t nc_cmp(struct namecache *x, struct namecache *y);
static void nc_trim_lru(void);
RB_GENERATE(namecache_rb, namecache, rb_entry, nc_cmp);

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
	if (ncp->vp != NULL) {
		kprintf(" -VN- reLEASE in nc_free()\n");
		vn_release(ncp->vp);
		ncp->vp = NULL;
	}
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
		ncstat.inactive++;
		ke_mutex_release(&ncp->mutex);
		if (!nested) {
			if (ncstat.inactive > ncstat.max_inactive)
				nc_trim_lru();
			ke_mutex_release(&lru_mutex);
		}
	} else if (nested) {
		ke_mutex_release(&ncp->mutex);
	}
	return NULL;
}

static void
do_trim(namecache_t *ncp)
{
	namecache_t *parent;

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
	ke_mutex_release(&ncp->mutex);
	nc_free(ncp);
	nc_release_internal(parent, true);
}

static void
nc_trim_lru(void)
{
	struct namecache *ncp;

loop:
	if (ncstat.inactive <= ncstat.max_inactive)
		return;

	ncp = TAILQ_FIRST(&lru_queue);
	TAILQ_REMOVE(&lru_queue, ncp, lru_entry);

	do_trim(ncp);

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
		found->n_mounts_over = 0;
		found->vfsp = nc->vfsp;
		RB_INIT(&found->entries);
		RB_INSERT(namecache_rb, &nc->entries, found);

		r = nc->vp->ops->lookup(nc->vp, &vnode, name);
		if (r != 0) {
			/* make a negative entry */
			found->refcnt = 0;
			found->vp = NULL;
			TAILQ_INSERT_TAIL(&lru_queue, found, lru_entry);
			ncstat.inactive++;
			ke_mutex_release(&nc->mutex);
			return -ENOENT;
		} else {
			found->refcnt = 1;
			found->vp = vnode;
			ke_mutex_release(&nc->mutex);
			*out = found;
			return 0;
		}
	} else if (found->vp == NULL) {
		/* negative entry */
		ke_mutex_release(&nc->mutex);
		return -ENOENT;
	}

	nc_retain(found);
	ke_mutex_release(&nc->mutex);

	*out = found;

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
	ncp->n_mounts_over = 0;
	ncp->parent = NULL;
	ncp->vfsp = vfs;
	RB_INIT(&ncp->entries);
	ncp->vp = vnode;

	root_nch.nc = ncp;
	root_nch.vfs = vfs;
}

void
nc_remove_vfs(vfs_t *vfs)
{
	namecache_t *ncp, *tmp;
	bool found_one;

	ke_wait(&lru_mutex, "nc_retain:lru_mutex", false, false, -1);
	do {
		found_one = false;

		TAILQ_FOREACH_SAFE (ncp, &lru_queue, lru_entry, tmp) {
			if (ncp->vfsp == vfs) {
				TAILQ_REMOVE(&lru_queue, ncp, lru_entry);
				found_one = true;
				do_trim(ncp);
			}
		}
	} while (found_one);
	ke_mutex_release(&lru_mutex);
}

enum nodeKind { kRoot, kChild, kLastChild };

static void
nc_dump_internal(namecache_handle_t nch, char *prefix, enum nodeKind kind,
    bool mountpoint)
{
#if 0
	const char *branch = "+-";
	const char *rcorner = "\\-";
	const char *vline = "| ";
#else
	const char *branch = "\e(0\x74\x71\e(B";  /* ├─ */
	const char *rcorner = "\e(0\x6d\x71\e(B"; /* └─ */
	const char *vline = "\e(0\x78\e(B";	  /* │ */
#endif
	namecache_t *child_ncp;
	char *newPrefix;

	if (kind == kRoot) {
		/* epsilon */
		newPrefix = prefix;
	}
	if (kind == kLastChild) {
		kprintf("%s%s", prefix, rcorner);
		kmem_asprintf(&newPrefix, "%s%s", prefix, "  ");
	} else if (kind == kChild) {
		kprintf("%s%s", prefix, branch);
		kmem_asprintf(&newPrefix, "%s%s ", prefix, vline);
	}

	ke_wait(&nch.nc->mutex, "nc_dump", false, false, -1);

	kprintf("%s [rc %d]\n", nch.nc->name == NULL ? "/" : nch.nc->name,
	    nch.nc->refcnt);

	RB_FOREACH (child_ncp, namecache_rb, &nch.nc->entries) {
		namecache_handle_t child_nch = { child_ncp, nch.vfs };

		kassert(child_ncp != nch.nc);

		nc_dump_internal(child_nch, newPrefix,
		    RB_NEXT(namecache_rb, &nch.nch->entries, child_ncp) ?
			kChild :
			kLastChild,
		    false);
	}

	ke_mutex_release(&nch.nc->mutex);

	if (newPrefix != prefix) {
		// kmem_strfree(prefix);
	}
}

void
nc_dump(void)
{
	nc_dump_internal(root_nch, "", kRoot, true);
}
