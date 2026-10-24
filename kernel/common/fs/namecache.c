/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file namecache.c
 * @brief Namecache.
 */
/*
 * @file vfs_namecache.c
 * @brief Namecache.
 *
 * Locking
 * -------
 *
 * The topology of the namecache is protected by the global topology_rwlock.
 * Holding this lock for writing allows modification of the tree structure,
 * holding it for reading stabilises the tree (including preventing LRU cached
 * entries from being evicted).
 *
 * The LRU queues are guarded by the lru_mutex.
 *
 * LRU
 * ---
 *
 * Every attached (i.e. is a child of a parent) namecache entry is on either the
 * positive or negative LRU queue, which retains the namecache. Therefore any
 * attached entry's reference count is always > 1. Since roots have no parent
 * they are not on an LRU queue.
 *
 * On retain, an accessed bit is set. The idea is that when there is an ager, it
 * will do something like CLOCK, and CAS the refcount of entries no one holds
 * from 1 to 0, then dissociate them under the topology lock held write.
 *
 * At dissociation from the tree, the entry is taken off its LRU queue and
 * released. The last release of a dissociated namecache frees it.
 *
 * The ordering is: topology_rwlock -> lru_mutex.
 *
 * Namecache resolution (from negative to positive) is done with the topology
 * lock released and the needful namecaches busied instead. People encountering
 * a busy namecache during lookup should wait for it to no longer be busy.
 *
 */

#include <sys/errno.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>
#include <sys/krx_atomic.h>
#include <sys/krx_vfs.h>
#include <sys/vnode.h>
#include <sys/tree.h>

#include <libkern/lib.h>
#include <libkern/queue.h>
#include <stdatomic.h>

static void nc_free(namecache_t *nc);

#define MNT_HASH_NBUCKETS 16
static LIST_HEAD(vfs_hash_bucket, vfs) vfs_hash[MNT_HASH_NBUCKETS];
namecache_handle_t root_nch;
static krwlock_t topology_lock;

static size_t lru_pos_n = 0, lru_neg_n = 0;
static TAILQ_HEAD(namecache_lru_queue, namecache)
    lru_pos_queue = TAILQ_HEAD_INITIALIZER(lru_pos_queue),
    lru_neg_queue = TAILQ_HEAD_INITIALIZER(lru_neg_queue);
static kmutex_t lru_mutex;

static size_t
nc_namelen(namecache_t *nc)
{
	return (size_t)(nc->key >> 32);
}

static int64_t
nc_cmp(struct namecache *x, struct namecache *y)
{
	if (x->key < y->key)
		return -1;
	else if (x->key > y->key)
		return 1;
	return memcmp(x->name, y->name, nc_namelen(x));
}

RB_GENERATE(namecache_rb, namecache, sib_rblink, nc_cmp);

static uint64_t
nc_hash(const char *str, size_t len)
{
	return fnv1a_hash32(str, len) | ((uint64_t)len << 32);
}

static inline uint32_t
nc_flags(namecache_t *nc)
{
	return atomic_load_explicit(&nc->flags, memory_order_relaxed);
}

static inline bool
nc_test(namecache_t *nc, uint32_t flag)
{
	return (nc_flags(nc) & flag) != 0;
}

/* (topology lock must be held as writer for anything but NC_ACCESSED) */
static inline void
nc_set(namecache_t *nc, uint32_t flag)
{
	atomic_fetch_or_explicit(&nc->flags, flag, memory_order_relaxed);
}

static inline void
nc_clear(namecache_t *nc, uint32_t flag)
{
	atomic_fetch_and_explicit(&nc->flags, ~flag, memory_order_relaxed);
}

static inline unsigned
nc_mounts_over(namecache_t *nc)
{
	return (nc_flags(nc) & NC_MOUNTS_MASK) >> NC_MOUNTS_SHIFT;
}

/*
 * An existence guarantee on nc is required to call this.
 * This can be either holding the topology_rwlock (shared is fine)
 * or the nc refcount being anyway > 1.
 */
static namecache_t *
nc_retain(namecache_t *nc)
{
	uint32_t old = atomic_fetch_add_explicit(&nc->refcnt, 1,
	    memory_order_relaxed);

	kassert(old != 0, "nc_retain of a frozen namecache");
	(void)old;

	if (!nc_test(nc, NC_ACCESSED)) /* try to avoid needless cache dirty */
		nc_set(nc, NC_ACCESSED);

	return nc;
}

/*
 * Topology lock may be held unless only if this is NOT the last reference to a
 * positive entry (in case of vnode release.)
 */
static void
nc_release(namecache_t *nc)
{
	uint32_t old;

	if (nc == NULL)
		return;

	old = atomic_fetch_sub_explicit(&nc->refcnt, 1, memory_order_release);
	kassert(old != 0);
	if (old == 1) {
		atomic_thread_fence(memory_order_acquire);
		nc_free(nc);
	}
}

static struct namecache_lru_queue *
lru_queue_for(namecache_t *nc, size_t **n_out)
{
	if (nc->vp == NULL) {
		*n_out = &lru_neg_n;
		return &lru_neg_queue;
	} else {
		*n_out = &lru_pos_n;
		return &lru_pos_queue;
	}
}

/*
 * Place an attached entry on an LRU queue, which then retains a reference.
 * (topology lock, write)
 */
static void
lru_insert(namecache_t *nc)
{
	struct namecache_lru_queue *queue;
	size_t *n;

	kassert(!nc_test(nc, NC_ON_LRU));
	kassert(nc->parent != NULL);

	atomic_fetch_add_explicit(&nc->refcnt, 1, memory_order_relaxed);

	ke_mutex_enter(&lru_mutex, "lru_insert");
	queue = lru_queue_for(nc, &n);
	TAILQ_INSERT_TAIL(queue, nc, lru_qlink);
	(*n)++;
	ke_mutex_exit(&lru_mutex);

	nc_set(nc, NC_ON_LRU);
}

/*
 * Remove an entry from its LRU queue. The queue's reference is released, so
 * this must be called with caller holding another retain on the entry.
 * (topology lock must be held write)
 */
static void
lru_remove(namecache_t *nc)
{
	struct namecache_lru_queue *queue;
	size_t *n;
	uint32_t old;

	kassert(nc_test(nc, NC_ON_LRU));

	ke_mutex_enter(&lru_mutex, "lru_remove");
	queue = lru_queue_for(nc, &n);
	TAILQ_REMOVE(queue, nc, lru_qlink);
	(*n)--;
	ke_mutex_exit(&lru_mutex);

	nc_clear(nc, NC_ON_LRU);

	old = atomic_fetch_sub_explicit(&nc->refcnt, 1, memory_order_release);
	kassert(old > 1, "lru_remove dropped the last reference");
	(void)old;
}

/*
 * vn must be retained (the retain is taken).
 * (topology lock must be held write except for not-yet-published roots)
 */
static void
nc_set_vp(namecache_t *nc, vnode_t *vn)
{
	ipl_t ipl;

	kassert(nc->vp == NULL);

	if (nc_test(nc, NC_ON_LRU)) {
		/* now positive */
		ke_mutex_enter(&lru_mutex, "nc_set_vp");
		TAILQ_REMOVE(&lru_neg_queue, nc, lru_qlink);
		lru_neg_n--;
		TAILQ_INSERT_TAIL(&lru_pos_queue, nc, lru_qlink);
		lru_pos_n++;
		ke_mutex_exit(&lru_mutex);
	}

	ipl = ke_spinlock_enter(&vn->lock);
	nc->vp = vn;
	LIST_INSERT_HEAD(&vn->nc_refs, nc, vn_link);
	vn->nc_refs_n++;
	ke_spinlock_exit(&vn->lock, ipl);
}

/* returns the vnode for release */
static vnode_t *
nc_clear_vp(namecache_t *nc)
{
	vnode_t *vn = nc->vp;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&vn->lock);
	LIST_REMOVE(nc, vn_link);
	kassert(vn->nc_refs_n > 0);
	vn->nc_refs_n--;
	nc->vp = NULL;
	ke_spinlock_exit(&vn->lock, ipl);

	return vn;
}

int
nc_vnode_live_name(vnode_t *vn, vnode_t **parent_out, char *name,
    size_t namelen)
{
	namecache_t *nc;
	ipl_t ipl;
	int r = -ENOENT;

	/* stabilise parent and name fields */
	ke_rwlock_enter_read(&topology_lock, "nc_vnode_live_name");
	/* stabilise the vnode's nc list */
	ipl = ke_spinlock_enter(&vn->lock);
	LIST_FOREACH(nc, &vn->nc_refs, vn_link) {
		size_t len;

		/* roots and dissociated entries aren't names */
		if (nc->parent == NULL || nc->parent->vp == NULL)
			continue;

		len = nc_namelen(nc);
		if (len >= namelen)
			continue;

		memcpy(name, nc->name, len);
		name[len] = '\0';

		/* fine, the child namecache retains the parent namecache */
		*parent_out = vn_retain(nc->parent->vp);
		r = 0;
		break;
	}
	ke_spinlock_exit(&vn->lock, ipl);
	ke_rwlock_exit_read(&topology_lock);

	return r;
}

/*
 * Free an entry that's no longer referenced. It must by now be either
 * dissociated or a never published root, since the LRU queues retain.
 * (If there is a vnode, i.e. not a negative entry, then the topology lock
 * musn't be held as vnode release can block on writeback, network, etc.)
 */
static void
nc_free(namecache_t *nc)
{
	kassert(nc->parent == NULL);
	kassert(!nc_test(nc, NC_ON_LRU | NC_BUSY));

	if (nc->name != NULL)
		kmem_free((void *)nc->name, nc_namelen(nc) + 1);

	if (nc->vp != NULL)
		vn_release(nc_clear_vp(nc));

	kmem_free(nc, sizeof(namecache_t));
}

/* Allocate a new namecache entry with an initial retain. */
static namecache_t *
nc_alloc(const char *name, uint64_t key)
{
	namecache_t *nc = kmem_alloc(sizeof(namecache_t));

	atomic_store_explicit(&nc->refcnt, 1, memory_order_relaxed);
	atomic_store_explicit(&nc->flags, 0, memory_order_relaxed);
	nc->vp = NULL;
	nc->parent = NULL;
	RB_INIT(&nc->children);
	ke_waitq_init(&nc->busy_waitq);
	nc->name = name == NULL ? NULL : kmem_strdup(name);
	nc->key = key;

	return nc;
}

int
vfs_try_retain(vfs_t *vfs)
{
	uint32_t current = atomic_load_explicit(&vfs->opencnt,
	    memory_order_acquire);
	while (current != 1) {
		uint32_t desired = current + 2;

		if (current & 1)
			kfatal("Unexpected value of VFS refcnt %d\n", current);

		if (atomic_compare_exchange_weak_explicit(&vfs->opencnt,
			&current, desired, memory_order_release,
			memory_order_relaxed)) {
			return 0;
		}
	}

	/* unmount in progress */
	return -EBUSY;
}

void
vfs_release(vfs_t *vfs)
{
	uint32_t old;

	if (vfs == NULL)
		return;
	old = atomic_fetch_sub_explicit(&vfs->opencnt, 2, memory_order_release);
	kassert(old >= 2, "vfs_release: no reference to release");
	(void)old;
}

namecache_handle_t
nchandle_retain(namecache_handle_t in)
{
	kassert(vfs_try_retain(in.vfs) == 0);
	nc_retain(in.nc);
	return in;
}

namecache_handle_t
nchandle_release(namecache_handle_t in)
{
	nc_release(in.nc);
	vfs_release(in.vfs);
	return (namecache_handle_t) { NULL, NULL };
}

static unsigned int
nchandle_hash(namecache_handle_t handle)
{
	uintptr_t hash;

	hash = (uintptr_t)handle.nc * 31;
	hash ^= ((uintptr_t)handle.vfs * 31);

	hash = (hash >> 16) ^ (hash & 0xFFFF);

	return hash % MNT_HASH_NBUCKETS;
}

static inline bool
nchandle_eq(namecache_handle_t x, namecache_handle_t y)
{
	return x.nc == y.nc && x.vfs == y.vfs;
}

static vfs_t *
mountpoint_find(namecache_handle_t nch)
{
	struct vfs_hash_bucket *bucket = &vfs_hash[nchandle_hash(nch)];
	vfs_t *vfs_entry;

	LIST_FOREACH(vfs_entry, bucket, mountpoint_hash_entry) {
		if (nchandle_eq(vfs_entry->nchcovered, nch))
			return vfs_entry;
	}
	return NULL;
}

void
nc_makeroot(vfs_t *vfs, vnode_t *root_vn)
{
	namecache_t *root_nc;

	root_nc = nc_alloc(NULL, nc_hash(NULL, 0));
	nc_set_vp(root_nc, root_vn);

	kassert(root_nch.nc == NULL);
	kassert(root_nch.vfs == NULL);

	vfs->root_nc = root_nc;
	vfs->nchcovered = (namecache_handle_t) { .nc = NULL, .vfs = NULL };

	root_nch.nc = root_nc;
	root_nch.vfs = vfs; /* consumes the initial vfs refcount */

	ke_rwlock_init(&topology_lock);
	ke_mutex_init(&lru_mutex);
}

void
nc_domount(namecache_handle_t overnch, vfs_t *vfs, vnode_t *rootvn)
{
	namecache_t *rootnc;

	rootnc = nc_alloc(NULL, nc_hash(NULL, 0));
	nc_set_vp(rootnc, rootvn);
	vfs->root_nc = rootnc;

	ke_rwlock_enter_write(&topology_lock, "nc_domount");
	atomic_fetch_add_explicit(&overnch.nc->flags, 1u << NC_MOUNTS_SHIFT,
	    memory_order_relaxed);
	LIST_INSERT_HEAD(&vfs_hash[nchandle_hash(overnch)], vfs,
	    mountpoint_hash_entry);
	vfs->nchcovered = nchandle_retain(overnch);
	ke_rwlock_exit_write(&topology_lock);
}

/*
 * Wait for a busy entry to resolve and exits topology lock (which must be held
 * read.)
 * (nc must be retained)
 */
static void
nc_wait_for_busy_and_exit_topology(struct namecache *nc)
{
	kwaiter_t waiter;

	ke_waitq_prepare(&nc->busy_waitq, &waiter);
	ke_rwlock_exit_read(&topology_lock);
	ke_waitq_wait(&nc->busy_waitq, &waiter,
	    "nc_wait_for_busy_and_exit_topology()", false, ABSTIME_FOREVER);
}

static void
nc_wake_busy_waiters(struct namecache *nc)
{
	ke_waitq_wake_all(&nc->busy_waitq);
}

/*
 * Dissociate a namecache entry from the tree and take it off any LRU queue it
 * may be on.
 *
 * (topology lock must be held write. caller must have a reference on the entry)
 */
static void
nc_dissociate(namecache_t *nc)
{
	kassert(nc->parent != NULL); /* can't dissociate twice */

	RB_REMOVE(namecache_rb, &nc->parent->children, nc);
	nc_release(nc->parent);
	nc->parent = NULL;

	lru_remove(nc);
}

/* (topology lock must be held write) */
static void
nc_busy(namecache_t *nc)
{
	kassert(!nc_test(nc, NC_BUSY));
	nc_set(nc, NC_BUSY);
}

/* (topology lock must be held write) */
static void
nc_unbusy(namecache_t *nc)
{
	kassert(nc_test(nc, NC_BUSY));
	nc_clear(nc, NC_BUSY);
	nc_wake_busy_waiters(nc);
}

/*
 * Cross any mounts that might exist over *nch
 * The parent's vfs reference is borrowed but the namecache entry's reference is
 * consumed.
 * (topology lock must be held read)
 */
static void
nc_cross_mounts(namecache_handle_t *nch, vfs_t *borrowed)
{
	while (nc_mounts_over(nch->nc) > 0) {
		vfs_t *vfs = mountpoint_find(*nch);
		namecache_handle_t vfsroot;

		if (vfs == NULL)
			break;

		/* again, fine, no lockless lookup yet */
		kassert(vfs_try_retain(vfs) == 0);
		vfsroot.nc = nc_retain(vfs->root_nc);
		vfsroot.vfs = vfs;

		/* mountpoint are attached, so it can't be the last release */
		if (nch->vfs == borrowed)
			nc_release(nch->nc);
		else
			nchandle_release(*nch);
		*nch = vfsroot;
	}
}

/*
 * since nc_lookup/nc_create borrow parent vfs reference unless mount was
 * crossed. TODO: is that an accident waiting to happen?
 */
static void
nc_release_child(namecache_handle_t parent, namecache_handle_t child)
{
	nc_release(child.nc);
	if (child.vfs != parent.vfs)
		vfs_release(child.vfs);
}

static void
nc_advance(namecache_handle_t *nch, namecache_handle_t child)
{
	if (child.vfs == nch->vfs)
		nc_release(nch->nc); /* borrowed VFS reference */
	else
		nchandle_release(*nch);
	*nch = child;
}

/*
 * Look up component `name` in the directory `parent`, traversing any mounts.
 * On success the found entry is written to *out - the VFS reference is BORROWED
 * from `parent` unless mounts were crossed. (use nc_advance())
 */
static int
nc_lookup(namecache_handle_t parent, namecache_handle_t *out, const char *name,
    bool allow_neg)
{
	namecache_t *nc = parent.nc;
	bool writelocked = false;
	vnode_t *vn;
	struct namecache *found, key;
	int r;

	key.name = (char *)name;
	key.key = nc_hash(name, strlen(name));

	ke_rwlock_enter_read(&topology_lock, "nc_lookup");

retry:
	found = RB_FIND(namecache_rb, &nc->children, &key);

	if (found != NULL) {
		if (nc_test(found, NC_BUSY)) {
			if (writelocked) {
				ke_rwlock_downgrade(&topology_lock);
				writelocked = false;
			}

			nc_retain(found);
			nc_wait_for_busy_and_exit_topology(found);
			/*
			 * While we waited the entry might have been dissociated
			 * and ours might be the last reference. Since we hold
			 * no lock the possible vnode release is fine.
			 */
			nc_release(found);
			ke_rwlock_enter_read(&topology_lock, "nc_lookup: retry");
			goto retry;
		} else if (found->vp == NULL && !allow_neg) {
			/* negative entry */
			r = -ENOENT;
		} else {
			out->nc = nc_retain(found);
			out->vfs = parent.vfs;
			nc_cross_mounts(out, parent.vfs);
			r = 0;
		}

		if (writelocked)
			ke_rwlock_exit_write(&topology_lock);
		else
			ke_rwlock_exit_read(&topology_lock);
		return r;
	}

	if (!writelocked) {
		writelocked = true;
		if (!ke_rwlock_tryupgrade(&topology_lock)) {
			ke_rwlock_exit_read(&topology_lock);
			ke_rwlock_enter_write(&topology_lock, "nc_lookup upgrade");
			goto retry;
		}
	}

	found = nc_alloc(name, key.key);
	found->parent = nc_retain(nc);
	RB_INSERT(namecache_rb, &nc->children, found);
	lru_insert(found); /* initially negative, lookup might change that */

	nc_busy(found);
	ke_rwlock_exit_write(&topology_lock);

	r = nc->vp->ops->lookup(nc->vp, name, &vn);

	ke_rwlock_enter_write(&topology_lock, "nc_lookup: relock");

	if (r == 0) {
		nc_set_vp(found, vn);
	} else if (r == -ENOENT && allow_neg) {
		r = 0;
	} else {
		nc_dissociate(found);
	}

	nc_unbusy(found);

	if (r == 0) {
		out->nc = found;
		out->vfs = parent.vfs;
		ke_rwlock_exit_write(&topology_lock);
	} else {
		ke_rwlock_exit_write(&topology_lock);
		nc_release(found);
	}

	return r;
}

/*
 * Create component `name` in the directory `parent`.
 * see nc_lookup() (but note here no mountpoint traversal)
 */
static int
nc_create(namecache_handle_t parent, namecache_handle_t *out, const char *name,
    vattr_t *attr)
{
	namecache_t key, *nc;
	vnode_t *new_vn = NULL;
	bool created = false;
	int r;

	if (parent.nc->vp->type != VDIR)
		return -ENOTDIR;
	if (parent.nc->vp->ops->create == NULL)
		return -ENOTSUP;

	key.name = (char *)name;
	key.key = nc_hash(name, strlen(name));

retry:
	ke_rwlock_enter_write(&topology_lock, "nc_create");

	nc = RB_FIND(namecache_rb, &parent.nc->children, &key);
	if (nc != NULL) {
		nc_retain(nc);

		if (nc_test(nc, NC_BUSY)) {
			ke_rwlock_downgrade(&topology_lock);
			nc_wait_for_busy_and_exit_topology(nc);
			nc_release(nc);
			goto retry;
		}

		if (nc_mounts_over(nc) > 0) {
			nc_release(nc);
			ke_rwlock_exit_write(&topology_lock);
			return -EBUSY;
		}

		if (nc->vp != NULL) {
			nc_release(nc);
			ke_rwlock_exit_write(&topology_lock);
			return -EEXIST;
		}
	} else {
		nc = nc_alloc(name, key.key);
		nc->parent = nc_retain(parent.nc);
		RB_INSERT(namecache_rb, &parent.nc->children, nc);
		lru_insert(nc);
		created = true;
	}

	nc_busy(nc);
	ke_rwlock_exit_write(&topology_lock);

	r = parent.nc->vp->ops->create(parent.nc->vp, name, attr, &new_vn);

	ke_rwlock_enter_write(&topology_lock, "nc_create: relock");

	if (r == 0) {
		nc_set_vp(nc, new_vn);
		nc_unbusy(nc);
		ke_rwlock_exit_write(&topology_lock);
		out->nc = nc; /* retained */
		out->vfs = parent.vfs;
	} else {
		if (r == -EEXIST) {
			/* make away with the stale negative */
			if (nc->vp == NULL)
				nc_dissociate(nc);
		} else if (created) {
			/* make away with it */
			nc_dissociate(nc);
		}

		nc_unbusy(nc);
		ke_rwlock_exit_write(&topology_lock);

		if (new_vn != NULL)
			vn_release(new_vn);

		nc_release(nc);
	}

	return r;
}

struct lookup {
	TAILQ_HEAD(namepart_tailq, namepart) components;
};

struct namepart {
	TAILQ_ENTRY(namepart) tailq_entry;
	char *name;
	bool must_be_dir;
};

static int
split_path(struct lookup *out, char *path, struct namepart *after)
{
	char *last;
	bool trailing_slash = false;
	char *component;
	size_t pathlen = strlen(path);

	if (pathlen > 0 && path[pathlen - 1] == '/')
		trailing_slash = true;

	component = strtok_r(path, "/", &last);
	while (component != NULL) {
		struct namepart *np = kmem_alloc(sizeof(struct namepart));
		if (np == NULL) {
			return -ENOMEM;
		}
		np->must_be_dir = false;
		np->name = component;
		if (np->name == NULL) {
			kmem_free(np, sizeof(*np));
			return -ENOMEM;
		}

		if (after == NULL) {
			TAILQ_INSERT_TAIL(&out->components, np, tailq_entry);
		} else {
			TAILQ_INSERT_AFTER(&out->components, after, np,
			    tailq_entry);
			after = np;
		}

		component = strtok_r(NULL, "/", &last);
	}

	if (after == NULL)
		after = TAILQ_LAST(&out->components, namepart_tailq);

	if (trailing_slash && after != NULL)
		after->must_be_dir = true;

	return 0;
}

int
vfs_lookup_init(struct lookup_info *info, namecache_handle_t start,
    const char *path, enum lookup_flags flags)
{
	info->start = start;
	info->path = path;
	info->flags = flags;
	if (flags & LOOKUP_EXCL)
		info->flags |= LOOKUP_NOFOLLOW_FINAL;
	info->create_attr = NULL;
	info->result = NCH_NULL;
	info->did_create = false;
	return 0;
}

/*
 * Lookup ".." in `nch`.
 * If nch's entry was dissociated in the meantime then returns -ENOENT.
 */
static int
nc_dotdot(namecache_handle_t *nch)
{
	namecache_t *parent;

	ke_rwlock_enter_read(&topology_lock, "nc_dotdot");

	/* follow the nchcovered chain */
	if (nch->nc == nch->vfs->root_nc) {
		namecache_handle_t mp = nch->vfs->nchcovered;

		if (mp.nc == NULL) {
			/* root vfs, .. is itself */
			ke_rwlock_exit_read(&topology_lock);
			return 0;
		}

		/* mp is the mountpoint entry in the parent mount */
		nchandle_retain(mp);
		/* being a root, this can never be the last release */
		nchandle_release(*nch);
		*nch = mp;
	}

	/* finally get the parent of this nc */
	if (nch->nc == nch->vfs->root_nc) {
		ke_rwlock_exit_read(&topology_lock);
		return 0;
	}

	parent = nch->nc->parent;
	if (parent == NULL) {
		ke_rwlock_exit_read(&topology_lock);
		return -ENOENT;
	}

	nc_retain(parent);
	ke_rwlock_exit_read(&topology_lock);

	nc_release(nch->nc);
	nch->nc = parent;

	/* nch->vfs unchanged */

	return 0;
}

int
vfs_lookup(struct lookup_info *info)
{
	struct lookup state;
	namecache_handle_t nch;
	struct namepart *np, *tmp;
	char *pathcpy, *linkpaths[8];
	size_t pathlen;
	size_t nlinks = 0;
	int r = 0;

	TAILQ_INIT(&state.components);

	pathcpy = kmem_strdup(info->path);
	pathlen = strlen(info->path) + 1;
	r = split_path(&state, pathcpy, NULL);
	if (r != 0)
		return r;

	if (*pathcpy == '/')
		nch = nchandle_retain(root_nch);
	else
		nch = nchandle_retain(info->start);

	for (np = TAILQ_FIRST(&state.components); np != NULL;
	    np = TAILQ_NEXT(np, tailq_entry)) {
		namecache_handle_t next;
		bool is_final = TAILQ_NEXT(np, tailq_entry) == NULL;

		if (is_final && info->flags & LOOKUP_2NDLAST)
			break;

		if (strcmp(np->name, ".") == 0) {
			/* note: ought to check if is dir */
			continue;
		} else if (strcmp(np->name, "..") == 0) {
			r = nc_dotdot(&nch);
			if (r != 0)
				break;
			continue;
		}

		if (nch.nc->vp->type != VDIR) {
			r = -ENOTDIR;
			break;
		}

		r = nc_lookup(nch, &next, np->name,
		    is_final && (info->flags & LOOKUP_ALLOW_NEG));
		if (r == -ENOENT && is_final && (info->flags & LOOKUP_CREATE)) {
			r = nc_create(nch, &next, np->name, info->create_attr);
			if (r == 0) {
				info->did_create = true;
			} else if (r == -EEXIST &&
			    !(info->flags & LOOKUP_EXCL)) {
				/*
				 * Stale negative. nc_create() has already made
				 * away with it so try again.
				 */
				r = nc_lookup(nch, &next, np->name,
				    is_final &&
					(info->flags & LOOKUP_ALLOW_NEG));
				if (r != 0)
					break;
			} else {
				break;
			}
		} else if (r != 0) {
			break;
		}

		if (is_final && (info->flags & LOOKUP_EXCL) &&
		    !info->did_create && next.nc->vp != NULL) {
			nc_release_child(nch, next);
			r = -EEXIST;
			break;
		}

		if (next.nc->vp != NULL && next.nc->vp->type == VLNK &&
		    !(is_final && (info->flags & LOOKUP_NOFOLLOW_FINAL))) {
			char *buf;

			if (nlinks + 1 > 8) {
				nc_release_child(nch, next);
				r = -ELOOP;
				goto out;
			}

			buf = linkpaths[nlinks++] = kmem_alloc(256);
			if (buf == NULL) {
				nc_release_child(nch, next);
				r = -ENOMEM;
				goto out;
			}

			r = VOP_READLINK(next.nc->vp, buf, 255);
			nc_release_child(nch, next);
			if (r < 0) {
				kmem_free(buf, 256);
				nlinks--;
				goto out;
			}

			buf[r] = '\0';

			r = split_path(&state, buf, np);
			if (r != 0)
				goto out;

			if (buf[0] == '/') {
				nchandle_release(nch);
				nch = nchandle_retain(root_nch);
			}

			continue;
		}

		nc_advance(&nch, next);
	}

out:
	TAILQ_FOREACH_SAFE(np, &state.components, tailq_entry, tmp)
		kmem_free(np, sizeof(*np));

	for (int i = 0; i < nlinks; i++)
		kmem_free(linkpaths[i], 256);

	kmem_free(pathcpy, pathlen);

	if (r == 0)
		info->result = nch;
	else
		nchandle_release(nch);

	return r;
}

int
vfs_lookup_simple(namecache_handle_t start, namecache_handle_t *out,
    const char *path, enum lookup_flags flags)
{
	struct lookup_info info;
	int r;

	r = vfs_lookup_init(&info, start, path, flags);
	if (r != 0)
		return r;

	r = vfs_lookup(&info);
	if (r != 0)
		return r;

	*out = info.result;

	return 0;
}

int
nc_link(namecache_handle_t dirnch, vnode_t *target_vn, const char *name)
{
	namecache_handle_t dst;
	int r;

	/* can't link into a non-dir */
	if (dirnch.nc->vp->type != VDIR)
		return -ENOTDIR;

	/* need a link op */
	if (dirnch.nc->vp->ops->link == NULL)
		return -ENOTSUP;

	/* dirs are not linkable */
	if (target_vn->type == VDIR)
		return -EPERM;

retry:
	r = vfs_lookup_simple(dirnch, &dst, name,
	    LOOKUP_NOFOLLOW_FINAL | LOOKUP_ALLOW_NEG);
	if (r != 0)
		return r;

	ke_rwlock_enter_write(&topology_lock, "nc_link");

	if (nc_test(dst.nc, NC_BUSY)) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy_and_exit_topology(dst.nc);
		nchandle_release(dst);
		goto retry;
	}

	if (strcmp(dst.nc->name, name) != 0 || dst.nc->parent != dirnch.nc) {
		ke_rwlock_exit_write(&topology_lock);
		nchandle_release(dst);
		goto retry;
	}

	if (nc_mounts_over(dst.nc) > 0) {
		r = -EBUSY;
		goto out;
	}

	if (dst.nc->vp != NULL) {
		r = -EEXIST;
		goto out;
	}

	nc_busy(dst.nc);

	ke_rwlock_exit_write(&topology_lock);

	r = dirnch.nc->vp->ops->link(dirnch.nc->vp, target_vn, name);

	ke_rwlock_enter_write(&topology_lock, "nc_link: relock topology_rwlock");
	if (r == 0) {
		nc_set_vp(dst.nc, vn_retain(target_vn));
	} else if (r == -EEXIST) {
		nc_dissociate(dst.nc); /* the negative entry seems stale */
	}

	nc_unbusy(dst.nc);
out:
	ke_rwlock_exit_write(&topology_lock);
	nchandle_release(dst);
	return r;
}

int
nc_remove(namecache_handle_t dirnch, const char *name, bool isdir)
{
	int r;
	namecache_handle_t targnch;

	if (dirnch.nc->vp->type != VDIR)
		return -ENOTDIR;

	if ((isdir ? dirnch.nc->vp->ops->rmdir : dirnch.nc->vp->ops->remove) ==
	    NULL)
		return -ENOTSUP;

retry:
	r = vfs_lookup_simple(dirnch, &targnch, name, LOOKUP_NOFOLLOW_FINAL);
	if (r != 0)
		return r;

	ke_rwlock_enter_write(&topology_lock, "nc_remove");

	if (nc_test(targnch.nc, NC_BUSY)) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy_and_exit_topology(targnch.nc);
		nchandle_release(targnch);
		goto retry;
	}

	if (strcmp(targnch.nc->name, name) != 0 ||
	    targnch.nc->parent != dirnch.nc) {
		ke_rwlock_exit_write(&topology_lock);
		nchandle_release(targnch);
		goto retry;
	}

	if (isdir && targnch.nc->vp != NULL && targnch.nc->vp->type != VDIR) {
		r = -ENOTDIR;
		goto out;
	}

	if (!isdir && targnch.nc->vp != NULL && targnch.nc->vp->type == VDIR) {
		r = -EISDIR;
		goto out;
	}

	if (nc_mounts_over(targnch.nc) > 0) {
		r = -EBUSY;
		goto out;
	}

	if (isdir) {
		/* TODO: consider a count of positive and negative children? */
		namecache_t *child;

		RB_FOREACH(child, namecache_rb, &targnch.nc->children) {
			if (child->vp != NULL) {
				r = -ENOTEMPTY;
				goto out;
			}
		}
	}

	nc_busy(targnch.nc);
	ke_rwlock_exit_write(&topology_lock);

	if (isdir)
		r = dirnch.nc->vp->ops->rmdir(dirnch.nc->vp, name,
		    targnch.nc->vp);
	else
		r = dirnch.nc->vp->ops->remove(dirnch.nc->vp, name,
		    targnch.nc->vp);

	ke_rwlock_enter_write(&topology_lock, "nc_remove: relock topology_lock");
	if (r == 0) {
		namecache_t *child;

		/* empty directory can still have negative children */

		while ((child = RB_MIN(namecache_rb, &targnch.nc->children)) !=
		    NULL) {
			kassert(child->vp == NULL);
			nc_retain(child); /* so the LRU's isn't last ref */
			nc_dissociate(child);
			nc_release(child); /* being negative, fine under lock */
		}

		nc_dissociate(targnch.nc);
	} else if (r == -ENOENT) {
		kdprintf("warning: nc_remove: remove failed in vfs layer");
	}

	nc_unbusy(targnch.nc);

out:
	ke_rwlock_exit_write(&topology_lock);
	nchandle_release(targnch);
	return r;
}

static void
nc_move(namecache_t *nc, namecache_t *new_dirnc, const char *new_name)
{
	namecache_t *old_dirnc = nc->parent;

	RB_REMOVE(namecache_rb, &old_dirnc->children, nc);

	nc->parent = nc_retain(new_dirnc);

	kmem_free(nc->name, nc_namelen(nc) + 1);
	nc->name = kmem_strdup(new_name);
	nc->key = nc_hash(new_name, strlen(new_name));

	RB_INSERT(namecache_rb, &new_dirnc->children, nc);

	nc_release(old_dirnc);
}

int
nc_rename(namecache_handle_t old_dirnch, const char *old_name,
    namecache_handle_t new_dirnch, const char *new_name)
{
	namecache_handle_t src, dst;
	int r;

	if (old_dirnch.nc->vp->type != VDIR || new_dirnch.nc->vp->type != VDIR)
		return -ENOTDIR;

	if (old_dirnch.nc == new_dirnch.nc && strcmp(old_name, new_name) == 0)
		return 0;

retry:
	r = vfs_lookup_simple(old_dirnch, &src, old_name,
	    LOOKUP_NOFOLLOW_FINAL);
	if (r != 0)
		return r;

	r = vfs_lookup_simple(new_dirnch, &dst, new_name,
	    LOOKUP_NOFOLLOW_FINAL | LOOKUP_ALLOW_NEG);
	if (r != 0) {
		nchandle_release(src);
		return r;
	}

	ke_rwlock_enter_write(&topology_lock, "nc_remove");

	if (nc_test(src.nc, NC_BUSY)) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy_and_exit_topology(src.nc);
		nchandle_release(src);
		nchandle_release(dst);
		goto retry;
	}

	if (nc_test(dst.nc, NC_BUSY)) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy_and_exit_topology(dst.nc);
		nchandle_release(src);
		nchandle_release(dst);
		goto retry;
	}

	/*
	 * revalidation because of unlocks:
	 * are the names still the same, and the parents still the same?
	 * if so, these are what we want to work with.
	 * otherwise, try again
	 */

	if (strcmp(src.nc->name, old_name) != 0 ||
	    strcmp(dst.nc->name, new_name) != 0 ||
	    src.nc->parent != old_dirnch.nc ||
	    dst.nc->parent != new_dirnch.nc) {
		ke_rwlock_exit_write(&topology_lock);
		nchandle_release(src);
		nchandle_release(dst);
		goto retry;
	}

	/* don't mess with mountpoints */
	if (nc_mounts_over(src.nc) > 0 || nc_mounts_over(dst.nc) > 0) {
		r = -EBUSY;
		goto out;
	}

	if (src.nc->vp != NULL && dst.nc->vp != NULL) {
		/* some proactive checks to save the vnode layer some bother. */

		/* can't replace dir with non-dir, nor vice versa */
		if (src.nc->vp->type == VDIR && dst.nc->vp->type != VDIR) {
			r = -ENOTDIR;
			goto out;
		}
		if (src.nc->vp->type != VDIR && dst.nc->vp->type == VDIR) {
			r = -EISDIR;
			goto out;
		}
	}

	if (src.nc->vp != NULL && src.nc->vp->type == VDIR) {
		/* check for a cycle, new parent must not be under src */
		namecache_t *check = new_dirnch.nc;
		while (check != NULL) {
			if (check == src.nc) {
				r = -EINVAL;
				goto out;
			}
			check = check->parent;
		}
	}

	nc_busy(src.nc);
	nc_busy(dst.nc);
	ke_rwlock_exit_write(&topology_lock);

	/* (if dst is negative, then its vp is NULL, nothing displaced) */
	r = VOP_RENAME(old_dirnch.nc->vp, old_name, src.nc->vp,
	    new_dirnch.nc->vp, new_name, dst.nc->vp);

	ke_rwlock_enter_write(&topology_lock, "nc_rename: relock");
	if (r == 0) {
		nc_dissociate(dst.nc);
		nc_move(src.nc, new_dirnch.nc, new_name);
	} else {
		kdprintf("warning: nc_rename: rename failed in vfs layer");
	}

	nc_unbusy(src.nc);
	nc_unbusy(dst.nc);

out:
	ke_rwlock_exit_write(&topology_lock);
	nchandle_release(src);
	nchandle_release(dst);
	return r;
}

enum nodeKind { ROOT, CHILD, LAST_CHILD };

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

	if (kind == ROOT) {
		/* epsilon */
		newPrefix = prefix;
	}
	if (kind == LAST_CHILD) {
		kdprintf("%s%s", prefix, rcorner);
		kmem_asprintf(&newPrefix, "%s%s", prefix, "  ");
	} else if (kind == CHILD) {
		kdprintf("%s%s", prefix, branch);
		kmem_asprintf(&newPrefix, "%s%s ", prefix, vline);
	}

	kdprintf("%s [rc %d]\n", nch.nc->name == NULL ? "/" : nch.nc->name,
	    nch.nc->refcnt);

	RB_FOREACH(child_ncp, namecache_rb, &nch.nc->children) {
		namecache_handle_t child_nch = { child_ncp, nch.vfs };

		kassert(child_ncp != nch.nc);

		nc_dump_internal(child_nch, newPrefix,
		    RB_NEXT(namecache_rb, &nch.nc->children, child_ncp) ?
			CHILD :
			LAST_CHILD,
		    false);
	}

	if (newPrefix != prefix) {
		// kmem_strfree(prefix);
	}
}

void
nc_dump(void)
{
	nc_dump_internal(root_nch, "", ROOT, true);
}
