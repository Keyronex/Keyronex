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
 * The standby positive and negative queues are guarded by their own mutexes.
 * Moving onto or off these queues acquires this mutex.
 *
 * Reclamation will proceed by taking the topology_rwlock exclusive, then
 *
 * The ordering is: topology_rwlock -> standby mutexes.
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

struct namecache_waiter {
	LIST_ENTRY(namecache_waiter) entry;
	kevent_t event;
};

static void nc_free(namecache_t *nc);

#define MNT_HASH_NBUCKETS 16
static LIST_HEAD(vfs_hash_bucket, vfs) vfs_hash[MNT_HASH_NBUCKETS];
namecache_handle_t root_nch;
static krwlock_t topology_lock;

static size_t standby_pos_n = 0, standby_neg_n = 0;
static TAILQ_HEAD(namecache_standby_queue, namecache)
    standby_pos_queue = TAILQ_HEAD_INITIALIZER(standby_pos_queue),
    standby_neg_queue = TAILQ_HEAD_INITIALIZER(standby_neg_queue);
static kmutex_t standby_pos_mutex, standby_neg_mutex;

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
	static const uint32_t fnv_prime = 0x01000193, fnv_basis = 0x811C9DC5;
	uint32_t hash = fnv_basis;

	for (size_t i = 0; i < len; i++) {
		hash ^= str[i];
		hash *= fnv_prime;
	}

	return hash | ((uint64_t)len << 32);
}

/*
 * An existence guarantee on nc is required to call this.
 * This can be either holding the topology_rwlock (shared is fine)
 * or the nc refcount being anyway > 1.
 */
static namecache_t *
nc_retain(namecache_t *nc)
{
	for (;;) {
		uint32_t current = atomic_load_explicit(&nc->refcnt,
		    memory_order_acquire);

	retry:
		if (current == 0) {
			kmutex_t *mutex;
			struct namecache_standby_queue *queue;
			size_t *queue_n;

			/* may need to take it off a standby queue */

			if (nc->vp == NULL) {
				mutex = &standby_neg_mutex;
				queue = &standby_neg_queue;
				queue_n = &standby_neg_n;
			} else {
				mutex = &standby_pos_mutex;
				queue = &standby_pos_queue;
				queue_n = &standby_pos_n;
			}

			ke_mutex_enter(mutex, "");

			current = atomic_load_explicit(&nc->refcnt,
			    memory_order_acquire);
			if (current != 0) {
				/* someone else ref'd it, retry */
				ke_mutex_exit(mutex);
				goto retry;
			}

			/*
			 * Otherwise (current == 0) we can just store; without
			 * holding the standby mutex, no one else can race with
			 * us here.
			 */
			atomic_store_explicit(&nc->refcnt, 1,
			    memory_order_release);
			TAILQ_REMOVE(queue, nc, standby_qlink);
			(*queue_n)--;
			ke_mutex_exit(mutex);
			return nc;

		} else if (atomic_compare_exchange_weak_explicit(&nc->refcnt,
			       &current, current + 1, memory_order_acq_rel,
			       memory_order_acquire)) {
			return nc;
		} else {
			/* CAS failed; retry */
			goto retry;
		}
	}
}

static void
nc_release(namecache_t *nc)
{
	if (nc == NULL)
		return;

	for (;;) {
		uint32_t current = atomic_load_explicit(&nc->refcnt,
		    memory_order_acquire);

	retry:

		if (current == 1) {
			kmutex_t *mutex;
			struct namecache_standby_queue *queue;
			size_t *queue_n;

			if (nc->parent == NULL) {
				/*
				 * No other way to reference it, and it's not
				 * bound for a standby queue.
				 */
				nc_free(nc);
				return;
			}

			/* may need to release to a standby queue, or free it */

			if (nc->vp == NULL) {
				mutex = &standby_neg_mutex;
				queue = &standby_neg_queue;
				queue_n = &standby_neg_n;
			} else {
				mutex = &standby_pos_mutex;
				queue = &standby_pos_queue;
				queue_n = &standby_pos_n;
			}

			ke_mutex_enter(mutex, "");

			/*
			 * We CAS because the refcount drop has been deferred.
			 * If there's any other way the namecache could be
			 * retained, that might have happened. I can't remember
			 * if there's any such way, but
			 */
			if (!atomic_compare_exchange_weak_explicit(&nc->refcnt,
				&current, 0, memory_order_release,
				memory_order_acquire)) {
				/* someone else ref'd it, try again */
				ke_mutex_exit(mutex);
				goto retry;
			}

			TAILQ_INSERT_TAIL(queue, nc, standby_qlink);
			(*queue_n)++;

			ke_mutex_exit(mutex);
			return;
		} else if (atomic_compare_exchange_weak_explicit(&nc->refcnt,
			       &current, current - 1, memory_order_release,
			       memory_order_acquire)) {
			return;
		} else {
			/* CAS failed; retry */
			goto retry;
		}
	}
}

static void
nc_free(namecache_t *nc)
{
	namecache_t *parent = nc->parent;

	if (parent != NULL)
		RB_REMOVE(namecache_rb, &parent->children, nc);

	if (nc->name != NULL)
		kmem_free((void *)nc->name, nc_namelen(nc) + 1);

	if (nc->vp != NULL) {
		vn_release(nc->vp);
		nc->vp = NULL;
	}

	kmem_free(nc, sizeof(namecache_t));

	if (parent != NULL)
		nc_release(parent);
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
	if (vfs == NULL)
		return;
	atomic_fetch_sub_explicit(&vfs->opencnt, 2, memory_order_release);
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

	root_nc = kmem_alloc(sizeof(namecache_t));
	atomic_store_explicit(&root_nc->refcnt, 1, memory_order_relaxed);
	root_nc->mounts_over_n = 0;
	root_nc->busy = false;
	root_nc->vp = root_vn;
	root_nc->parent = NULL;
	RB_INIT(&root_nc->children);
	root_nc->name = NULL;
	root_nc->key = nc_hash(root_nc->name, 0);

	kassert(root_nch.nc == NULL);
	kassert(root_nch.vfs == NULL);

	vfs->root_nc = root_nc;
	vfs->nchcovered = (namecache_handle_t) { .nc = NULL, .vfs = NULL };

	root_nch.nc = root_nc;
	root_nch.vfs = vfs; /* consumes the initial vfs refcount */

	ke_rwlock_init(&topology_lock);
	ke_mutex_init(&standby_pos_mutex);
	ke_mutex_init(&standby_neg_mutex);
}

void
nc_domount(namecache_handle_t overnch, vfs_t *vfs, vnode_t *rootvn)
{
	namecache_t *rootnc;

	rootnc = kmem_alloc(sizeof(namecache_t));
	atomic_store_explicit(&rootnc->refcnt, 1, memory_order_relaxed);
	rootnc->mounts_over_n = 0;
	rootnc->busy = false;
	rootnc->vp = rootvn;
	rootnc->parent = NULL;
	RB_INIT(&rootnc->children);
	rootnc->name = NULL;
	rootnc->key = nc_hash(rootnc->name, 0);
	vfs->root_nc = rootnc;

	ke_rwlock_enter_write(&topology_lock, "nc_domount");
	overnch.nc->mounts_over_n++;
	LIST_INSERT_HEAD(&vfs_hash[nchandle_hash(overnch)], vfs,
	    mountpoint_hash_entry);
	vfs->nchcovered = nchandle_retain(overnch);
	ke_rwlock_exit_write(&topology_lock);
}

static void
nc_wait_for_busy(struct namecache *nc)
{
	struct namecache_waiter waiter;
	ipl_t ipl;

	ke_event_init(&waiter.event, false);

	ipl = ke_spinlock_enter(&nc->waiters_lock);
	LIST_INSERT_HEAD(&nc->waiters, &waiter, entry);
	ke_spinlock_exit(&nc->waiters_lock, ipl);

	ke_rwlock_exit_read(&topology_lock);
	ke_wait1(&waiter.event, "nc_wait_for_busy()", false,
	    ABSTIME_FOREVER);
	ke_rwlock_enter_read(&topology_lock, "after nc_wait_for_busy");
}

static void
nc_wake_busy_waiters(struct namecache *nc)
{
	struct namecache_waiter *waiter, *tmp;
	ipl_t ipl;

	ipl = ke_spinlock_enter(&nc->waiters_lock);
	LIST_FOREACH_SAFE(waiter, &nc->waiters, entry, tmp) {
		ke_event_set_signalled(&waiter->event, true);
		LIST_REMOVE(waiter, entry);
	}
	ke_spinlock_exit(&nc->waiters_lock, ipl);
}

static void
nc_dissociate(namecache_t *nc)
{
	kassert(nc->parent != NULL); /* can't dissociate twice */

	RB_REMOVE(namecache_rb, &nc->parent->children, nc);
	nc_release(nc->parent);
	nc->parent = NULL;
}

static void
nc_busy(namecache_t *nc)
{
	nc->busy = true;
	ke_spinlock_init(&nc->waiters_lock);
	LIST_INIT(&nc->waiters);
}

int
nc_lookup(struct namecache *nc, struct namecache **out, const char *name,
    bool allow_neg)
{
	bool writelocked = false;
	vnode_t *vn;
	struct namecache *found, key;
	int r;

	key.name = (char *)name;
	key.key = nc_hash(name, name == NULL ? 0 : strlen(name));

retry:
	found = RB_FIND(namecache_rb, &nc->children, &key);

	if (found != NULL) {
		if (found->busy) {
			if (writelocked) {
				ke_rwlock_downgrade(&topology_lock);
				writelocked = false;
			}

			nc_retain(found);
			nc_wait_for_busy(found);
			goto retry;

		} else if (found->vp == NULL && !allow_neg) {
			/* negative entry */
			return -ENOENT;
		} else {
			nc_retain(found);
			*out = found;
			return 0;
		}
	}

	if (!writelocked) {
		writelocked = true;
		if (!ke_rwlock_tryupgrade(&topology_lock)) {
			ke_rwlock_exit_read(&topology_lock);
			ke_rwlock_enter_write(&topology_lock, "nc_lookup upgrade");
			goto retry;
		}
	}

	found = kmem_alloc(sizeof(namecache_t));

	atomic_store_explicit(&found->refcnt, 1, memory_order_relaxed);
	found->name = name == NULL ? NULL : kmem_strdup(name);
	found->key = key.key;
	found->parent = nc_retain(nc);
	found->mounts_over_n = 0;
	found->vp = NULL;
	RB_INIT(&found->children);
	RB_INSERT(namecache_rb, &nc->children, found);

	nc_busy(found);
	ke_rwlock_exit_write(&topology_lock);

	r = nc->vp->ops->lookup(nc->vp, name, &vn);

	ke_rwlock_enter_write(&topology_lock, "nc_lookup: relock");

	if (r == 0) {
		found->vp = vn;
		*out = found;
	} else if (r == -ENOENT && allow_neg) {
		*out = found;
		r = 0;
	}

	found->busy = false;
	nc_wake_busy_waiters(found);
	ke_rwlock_downgrade(&topology_lock);

	return r;
}

static int
nc_create(namecache_t *parent, namecache_t **out, const char *name,
    vattr_t *attr)
{
	namecache_t key, *nc;
	vnode_t *new_vn = NULL;
	bool created = false;
	int r;

	if (parent->vp->type != VDIR)
		return -ENOTDIR;
	if (parent->vp->ops->create == NULL)
		return -ENOTSUP;

retry:
	if (!ke_rwlock_tryupgrade(&topology_lock)) {
		ke_rwlock_exit_read(&topology_lock);
		ke_rwlock_enter_write(&topology_lock, "nc_create: upgrade");
	}

	key.name = (char *)name;
	key.key = nc_hash(name, strlen(name));

	nc = RB_FIND(namecache_rb, &parent->children, &key);
	if (nc != NULL) {
		nc_retain(nc);

		if (nc->busy) {
			ke_rwlock_downgrade(&topology_lock);
			nc_wait_for_busy(nc);
			nc_release(nc);
			goto retry;
		}

		if (nc->mounts_over_n > 0) {
			nc_release(nc);
			ke_rwlock_downgrade(&topology_lock);
			return -EBUSY;
		}

		if (nc->vp != NULL) {
			nc_release(nc);
			ke_rwlock_downgrade(&topology_lock);
			return -EEXIST;
		}
	} else {
		nc = kmem_alloc(sizeof(*nc));
		nc->name = kmem_strdup(name);
		nc->key = key.key;
		nc->parent = nc_retain(parent);
		nc->mounts_over_n = 0;
		nc->vp = NULL;
		RB_INIT(&nc->children);
		atomic_store(&nc->refcnt, 1);

		RB_INSERT(namecache_rb, &parent->children, nc);
		created = true;
	}

	nc_busy(nc);
	ke_rwlock_exit_write(&topology_lock);

	r = parent->vp->ops->create(parent->vp, name, attr, &new_vn);

	ke_rwlock_enter_write(&topology_lock, "nc_create: relock");

	if (r == 0) {
		nc->vp = new_vn;
		nc->busy = false;
		nc_wake_busy_waiters(nc);
		*out = nc; /* retained */
	} else {
		if (r == -EEXIST) {
			/* make away with the stale negative */
			if (nc->vp == NULL)
				nc_dissociate(nc);
		} else if (created) {
			/* make away with it */
			nc_dissociate(nc);
		}

		if (new_vn != NULL)
			vn_release(new_vn);

		nc->busy = false;
		nc_wake_busy_waiters(nc);
		nc_release(nc);
	}

	ke_rwlock_downgrade(&topology_lock);

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
	info->create_attr = NULL;
	info->result = NCH_NULL;
	info->did_create = false;
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

	ke_rwlock_enter_read(&topology_lock, "nc_lookup");

	if (*pathcpy == '/')
		nch = nchandle_retain(root_nch);
	else
		nch = nchandle_retain(info->start);


	for (np = TAILQ_FIRST(&state.components); np != NULL;
	    np = TAILQ_NEXT(np, tailq_entry)) {
		namecache_handle_t next_nch;
		bool is_final = TAILQ_NEXT(np, tailq_entry) == NULL;

		if (is_final && info->flags & LOOKUP_2NDLAST)
			break;

		if (strcmp(np->name, ".") == 0) {
			/* note: ought to check if is dir */
			continue;
		} else if (strcmp(np->name, "..") == 0) {

			/* follow the nchcovered chain */
			if (nch.nc == nch.vfs->root_nc) {
				namecache_handle_t mp = nch.vfs->nchcovered;

				if (mp.nc != NULL) {
					/*
					 * mp is the mountpoint entry in the
					 * parent mount
					 */
					nchandle_retain(mp);
					nchandle_release(nch);
					nch = mp;
				} else {
					/* root vfs, .. is itself */
					continue;
				}
			}

			/* finally get the parent of this nc */
			if (nch.nc != nch.vfs->root_nc &&
			    nch.nc->parent != NULL) {
				namecache_t *p = nc_retain(nch.nc->parent);
				nc_release(nch.nc);
				nch.nc = p;
				/* nch.vfs unchanged */
			}

			continue;
		}

		next_nch.vfs = nch.vfs;

		if (nch.nc->vp->type != VDIR) {
			r = -ENOTDIR;
			break;
		}

		r = nc_lookup(nch.nc, &next_nch.nc, np->name,
		    is_final && (info->flags & LOOKUP_ALLOW_NEG));
		if (r == -ENOENT && is_final && (info->flags & LOOKUP_CREATE)) {
			r = nc_create(nch.nc, &next_nch.nc, np->name,
			    info->create_attr);
			if (r == 0)
				info->did_create = true;
			else
				break;
		} else if (r != 0) {
			break;
		}

		while (next_nch.nc->mounts_over_n > 0) {
			vfs_t *vfs;

			vfs = mountpoint_find(next_nch);
			if (vfs != NULL) {
				namecache_handle_t vfsroot;

				/* again, fine, no lockless lookup yet */
				kassert(vfs_try_retain(vfs) == 0);

				vfsroot.nc = nc_retain(vfs->root_nc);
				vfsroot.vfs = vfs;

				nchandle_release(next_nch);

				next_nch = vfsroot;
			} else {
				break;
			}
		}

		nc_release(nch.nc);
		nch = next_nch;

		if (nch.nc->vp != NULL && nch.nc->vp->type == VLNK) {
			char *buf;
			bool is_final_lnk = TAILQ_NEXT(np, tailq_entry) == NULL;

			if (is_final_lnk &&
			    (info->flags & LOOKUP_NOFOLLOW_FINAL))
				continue;

			if (nlinks + 1 > 8) {
				r = -ELOOP;
				goto out;
			}

			buf = linkpaths[nlinks++] = kmem_alloc(256);
			if (buf == NULL) {
				r = -ENOMEM;
				goto out;
			}

			r = VOP_READLINK(nch.nc->vp, buf, 255);
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
			} else {
				namecache_t *parent = nc_retain(nch.nc->parent);
				nc_release(nch.nc);
				nch.nc = parent;
			}
		}
	}

out:
	ke_rwlock_exit_read(&topology_lock);

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

	if (dst.nc->busy) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy(dst.nc);
		ke_rwlock_exit_read(&topology_lock);
		nchandle_release(dst);
		goto retry;
	}

	if (strcmp(dst.nc->name, name) != 0 || dst.nc->parent != dirnch.nc) {
		ke_rwlock_exit_write(&topology_lock);
		nchandle_release(dst);
		goto retry;
	}

	if (dst.nc->mounts_over_n > 0) {
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
		dst.nc->vp = vn_retain(target_vn);
	} else if (r == -EEXIST) {
		nc_dissociate(dst.nc); /* the negative entry seems stale */
	}

	dst.nc->busy = false;
	nc_wake_busy_waiters(dst.nc);
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

	if (dirnch.nc->vp->ops->remove == NULL)
		return -ENOTSUP;

retry:
	r = vfs_lookup_simple(dirnch, &targnch, name, LOOKUP_NOFOLLOW_FINAL);
	if (r != 0)
		return r;

	ke_rwlock_enter_write(&topology_lock, "nc_remove");

	if (targnch.nc->busy) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy(targnch.nc);
		ke_rwlock_exit_read(&topology_lock);
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

	if (targnch.nc->mounts_over_n > 0) {
		r = -EBUSY;
		goto out;
	}

	if (isdir && !RB_EMPTY(&targnch.nc->children)) {
		r = -ENOTEMPTY;
		goto out;
	}

	nc_busy(targnch.nc);
	ke_rwlock_exit_write(&topology_lock);

	r = dirnch.nc->vp->ops->remove(dirnch.nc->vp, name);

	ke_rwlock_enter_write(&topology_lock, "nc_remove: relock topology_lock");
	if (r == 0) {
		nc_dissociate(targnch.nc);
	} else if (r == -ENOENT) {
		kdprintf("warning: nc_remove: remove failed in vfs layer");
	}

	targnch.nc->busy = false;
	nc_wake_busy_waiters(targnch.nc);

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

	if (src.nc->busy) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy(src.nc);
		ke_rwlock_exit_read(&topology_lock);
		nchandle_release(src);
		nchandle_release(dst);
		goto retry;
	}

	if (dst.nc->busy) {
		ke_rwlock_downgrade(&topology_lock);
		nc_wait_for_busy(dst.nc);
		ke_rwlock_exit_read(&topology_lock);
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
	if (src.nc->mounts_over_n > 0 || dst.nc->mounts_over_n > 0) {
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

	r = VOP_RENAME(old_dirnch.nc->vp, old_name, new_dirnch.nc->vp,
	    new_name);

	ke_rwlock_enter_write(&topology_lock, "nc_rename: relock");
	if (r == 0) {
		nc_dissociate(dst.nc);
		nc_move(src.nc, new_dirnch.nc, new_name);
	} else {
		kdprintf("warning: nc_rename: rename failed in vfs layer");
	}

	src.nc->busy = false;
	dst.nc->busy = false;
	nc_wake_busy_waiters(src.nc);
	nc_wake_busy_waiters(dst.nc);

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
