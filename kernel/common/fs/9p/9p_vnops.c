/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file 9p_vnops.c
 * @brief Vnode operations for 9p.
 *
 * The vcache is used - indexed by qid.path. (qemu's qid.version is just mtim!)
 */

/* FIXME: Old code, needs review!! */

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>
#include <sys/krx_vfs.h>
#include <sys/stat.h>
#include <sys/vn_idcache.h>
#include <sys/vnode.h>

#include <dirent.h>
#include <inttypes.h>
#include <libkern/lib.h>

#include "9pbuf.h"
#include "sys/vm.h"

typedef uint32_t fid_t;

#define NOFID ((fid_t)-1)
#define NOGEN ((uint64_t)-1)

/*
 * A 9p 'inode'
 *
 * open_fid_lock serialises lazy opening of the paging_fid.
 * Pager I/O and getattr read it atomically without taking the
 */
struct ninep_node {
	vn_idcache_entry_t idc_entry;
	struct ninep_qid qid;	/* immutable, qid.path is the idcache key */
	fid_t	fid,		/* unopened FID identifying the node */
		paging_fid;	/* open FID (for file I/O and readdir) or 0 */
	kmutex_t open_fid_lock;
	bool paging_fid_rdonly;	/* open fid open only for read */
	bool unlinked;		/* set by node_evict(), no lock to read */
	uint64_t gen;		/* host inode gen, or NOGEN */
	vnode_t *vnode;
	krwlock_t rwlock, paging_rwlock;
	vattr_t vattr;

#if 1
	char *name;
#endif
};

struct ninepfs_state {
	vfs_t *vfs;
	atomic_ushort req_tag;
	atomic_uint fid_counter;
	vn_idcache_t idcache;
	struct ninep_node *root_node;
	struct vnode *provider;

	/* debug counters */
	atomic_uint fids_allocated, fids_clunked, fids_unused;
	atomic_uint nodes_created, nodes_freed, nodes_torn_down;
};

/* TODO get rid of me when not needed for debugging */
struct ninepfs_state *ninep_debug_state;

#define VTO9(VNODE)   ((struct ninep_node *)(VNODE)->fsprivate_1)
#define VTO9FS(VNODE) ((struct ninepfs_state *)(VNODE)->vfs->fsprivate_1)

static struct vnode_ops ninep_vnops;

static int do_getattr(struct ninepfs_state *fs, fid_t fid, vattr_t *vattr,
    bool want_gen);
static void node_evict(struct ninepfs_state *fs, struct ninep_node *node);

static fid_t
fid_allocate(struct ninepfs_state *fs)
{
	fid_t fid;

	do {
		fid = atomic_fetch_add_explicit(&fs->fid_counter, 1,
		    memory_order_relaxed);
	} while (fid == 0 || fid == NOFID);

	atomic_fetch_add_explicit(&fs->fids_allocated, 1,
	    memory_order_relaxed);

	return fid;
}

/*! Clunk a fid. (even if there is an error the FID is now invalid.) */
static int
fid_clunk(struct ninepfs_state *fs, fid_t fid)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	int r;

	kassert(fid != 0 && fid != NOFID);

	/* size[4] Tclunk tag[2] fid[4] */
	buf_in = ninep_buf_alloc("F");
	/* size[4] Rclunk tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pClunk;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pClunk + 1:
		r = 0;
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kdprintf("9pfs: Tclunk of fid %u failed: %u\n", fid, err);
		r = -err;
		break;
	}

	default:
		kfatal("9p error\n");
	}

	ninep_buf_free(buf_out);

	atomic_fetch_add_explicit(&fs->fids_clunked, 1, memory_order_relaxed);

	return r;
}

/*!
 * Frees a never published node (i.e. raced to insert into vcache or getattr
 * failed.)
 */
static void
node_free_unpublished(struct ninepfs_state *fs, struct ninep_node *node)
{
	bool frozen;

	frozen = vn_tryfreeze(node->vnode);
	kassert(frozen, "node_free_unpublished: vnode has references");
	vn_free(node->vnode);
	kmem_free(node, sizeof(*node));
	atomic_fetch_add_explicit(&fs->nodes_freed, 1, memory_order_relaxed);
}

/*!
 * Finds the ninep_node corresponding to a QID if it exists in the cache,
 * retaining its vnode if so, or, if not found, creates one.
 *
 * @retval 1 if an extant node was found; caller should clunk \p fid.
 * @retval 0 if a node was created; the node took over \p fid.
 * @retval <0 getattr of new node failed
 */
static int
node_for_qid(struct ninepfs_state *fs, struct ninep_node **out,
    struct ninep_qid qid, fid_t fid, vtype_t override_vtype)
{
	struct ninep_node *node;
	vnode_t *vn;
	int r;

	r = vn_idcache_find(&fs->idcache, &qid.path, sizeof(qid.path), &vn);
	if (r == 0) {
		node = VTO9(vn);
		kassert(node->qid.path == qid.path);

		/*
		 * Try to detect host reuse of the inode for this qid.path and
		 * enthustically evict our node if it did.
		 *
		 * qid.version is useless for this (QEMU's 9p it's just mtime)
		 * but qid.type and (if we have it) stat.gen are helpful.
		 */
		if (node->qid.type != qid.type) {
			r = -ESTALE;
		} else if (node->gen != NOGEN) {
			vattr_t fresh;

			r = do_getattr(fs, fid, &fresh, true);
			if (r == 0 && fresh.gen != NOGEN &&
			    fresh.gen != node->gen)
				r = -ESTALE;
		} else {
			r = 0;
		}

		if (r == -ESTALE) {
			kdprintf("9p: node for qid %" PRIu64 " is stale (host "
			    "reused the inode); evicting\n", qid.path);
			node_evict(fs, node);
			vn_release(vn);
			/* fall out and proceed */
		} else {
			/* be optimistic if getattr failed */
			*out = node;
			return 1;
		}
	} else {
		kassert(r == -ENOENT);
	}

	node = kmem_alloc(sizeof(struct ninep_node));
	node->qid = qid;
	node->fid = fid;
	node->paging_fid = 0;
	node->paging_fid_rdonly = false;
	node->unlinked = false;
	node->name = NULL;
	ke_rwlock_init(&node->rwlock);
	ke_rwlock_init(&node->paging_rwlock);
	ke_mutex_init(&node->open_fid_lock);
	atomic_fetch_add_explicit(&fs->nodes_created, 1, memory_order_relaxed);

	r = do_getattr(fs, fid, &node->vattr, true);
	node->gen = r == 0 ? node->vattr.gen : NOGEN;
	if (r != 0) {
		kdprintf("9p: node_for_qid: getattr failed: %d\n", r);
		kmem_free(node, sizeof(*node));
		atomic_fetch_add_explicit(&fs->nodes_freed, 1,
		    memory_order_relaxed);
		return r;
	}

	if (override_vtype != VNON)
		node->vattr.type = override_vtype;

	node->vnode = vn_alloc(fs->vfs, node->vattr.type, &ninep_vnops,
	    (uintptr_t)node);
	if (node->vattr.type == VREG)
		vm_vnobj_set_valid_length(node->vnode->file.vmobj,
		    node->vattr.size);

	vn_idcache_entry_init(&node->idc_entry, node->vnode, &node->qid.path,
	    sizeof(node->qid.path));

	for (;;) {
		struct ninep_node *existing;

		r = vn_idcache_insert(&fs->idcache, &node->idc_entry, &vn);
		if (r == 0)
			break;
		kassert(r == -EEXIST);

		existing = VTO9(vn);
		kassert(existing->qid.path == qid.path);

		if (existing->qid.type != qid.type ||
		    (existing->gen != NOGEN &&
			node->gen != NOGEN &&
			existing->gen != node->gen)) {
			/* stale node created meantime, get rid of it */
			kdprintf("9p: node for qid %" PRIu64 " is stale (host "
			    "reused the inode); evicting\n", qid.path);
			node_evict(fs, existing);
			vn_release(vn);
			continue;
		}

		node_free_unpublished(fs, node);
		*out = existing;
		return 1;
	}

	*out = node;

	return 0;
}

/*!
 * Take a node out of the vcache so that future lookup of the same qid.path make
 * a new node. Existing references to the old node remain (which might be
 * problematic.)
 */
static void
node_evict(struct ninepfs_state *fs, struct ninep_node *node)
{
	vn_idcache_remove(&fs->idcache, &node->idc_entry);
	node->unlinked = true;
}

/*!
 * As node_for_qid() but for a newly-created node. If there is an existing node
 * matching its identity then it must be stale so it is evicted.
 */
static int
node_for_new_qid(struct ninepfs_state *fs, struct ninep_node **out,
    struct ninep_qid qid, fid_t fid, vtype_t override_vtype)
{
	int r;

	r = node_for_qid(fs, out, qid, fid, override_vtype);
	if (r == 1) {
		node_evict(fs, *out);
		vn_release((*out)->vnode); /* node_for_qid's retain */
		r = node_for_qid(fs, out, qid, fid, override_vtype);
	}

	return r;
}

static int
fid_clone(struct ninepfs_state *fs, ninep_fid_t fid, ninep_fid_t new_fid)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	int r;

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFh");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("h");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu16(buf_in, 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwnames;
		ninep_buf_getu16(buf_out, &nwnames);
		kassert(nwnames == 0);
		r = 0;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		atomic_fetch_add_explicit(&fs->fids_unused, 1,
		    memory_order_relaxed);
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

/*!
 * Open an unopened FID
 * @retval 0 FID now opened
 * @retval -errno failed, FID unchanged, not opened
 */
static int
fid_open(struct ninepfs_state *fs, fid_t fid, uint32_t flags)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	int r;

	/* size[4] Tlopen tag[2] fid[4] flags[4] */
	buf_in = ninep_buf_alloc("Fd");
	/* size[4] Rlopen tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLopen;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addu32(buf_in, flags);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pLopen + 1:
		r = 0;
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kassert(err != 0);
		r = -err;
		break;
	}

	default:
		kfatal("9p error\n");
	}

	ninep_buf_free(buf_out);

	return r;
}

static int
fid_walk1(struct ninepfs_state *fs, fid_t fid, fid_t new_fid, const char *name,
    struct ninep_qid *qid_out)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	int r;

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFhS64");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("hQ");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu16(buf_in, 1);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwqid;
		struct ninep_qid qid;

		ninep_buf_getu16(buf_out, &nwqid);
		kassert(nwqid == 1);
		ninep_buf_getqid(buf_out, &qid);
		if (qid_out != NULL)
			*qid_out = qid;
		r = 0;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		kassert(r != 0);
		atomic_fetch_add_explicit(&fs->fids_unused, 1,
		    memory_order_relaxed);
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

/* Get a walk fid for a node, trying to reconstruct one if necessary. */
static int
node_walk_fid(struct ninepfs_state *fs, struct ninep_node *node, fid_t *out)
{
	fid_t fid, parent_fid, newfid;
	vnode_t *parent_vn;
	struct ninep_qid qid;
	char name[256];
	int r;

	fid = node->fid;
	if (fid != NOFID) {
		*out = fid;
		return 0;
	}

	if (node->unlinked)
		return -ENOENT;

	/*
	 * Try to reconstruct a walk fid by getting a known name of the node
	 * from the namecache and re-walking from the parent. Works in the
	 * common case.
	 */

	r = nc_vnode_live_name(node->vnode, &parent_vn, name, sizeof(name));
	if (r != 0)
		return -ENOENT;

	if (parent_vn->vfs != node->vnode->vfs) {
		/* parent is in another FS */
		vn_release(parent_vn);
		return -ENOENT;
	}

	r = node_walk_fid(fs, VTO9(parent_vn), &parent_fid);
	if (r != 0) {
		vn_release(parent_vn);
		return r;
	}

	newfid = fid_allocate(fs);
	r = fid_walk1(fs, parent_fid, newfid, name, &qid);
	vn_release(parent_vn);
	if (r != 0)
		return r;

	if (qid.path != node->qid.path) {
		/* name refers to another node now */
		fid_clunk(fs, newfid);
		return -ENOENT;
	}

	ke_rwlock_enter_write(&node->rwlock, "node_walk_fid");
	if (node->fid == NOFID)
		node->fid = newfid;
	else
		fid_clunk(fs, newfid);
	fid = node->fid;
	ke_rwlock_exit_write(&node->rwlock);

	*out = fid;
	return 0;
}

static int
node_make_paging_fid(struct ninepfs_state *fs, struct ninep_node *node)
{
	ninep_fid_t new_fid, walk_fid;
	bool rdonly = false;
	int r;

	if (node->vnode->type != VREG && node->vnode->type != VDIR)
		return -EBADF;

	ke_mutex_enter(&node->open_fid_lock, "node_make_paging_fid");

	if (node->paging_fid != 0) {
		ke_mutex_exit(&node->open_fid_lock);
		return 0;
	}

	r = node_walk_fid(fs, node, &walk_fid);
	if (r != 0) {
		ke_mutex_exit(&node->open_fid_lock);
		return r;
	}

	new_fid = fid_allocate(fs);

	r = fid_clone(fs, walk_fid, new_fid);
	if (r != 0) {
		kdprintf("9pfs: failed to clone fid for %s: %d\n", node->name,
		    r);
		ke_mutex_exit(&node->open_fid_lock);
		return r;
	}

	if (node->vnode->type == VDIR) {
		r = fid_open(fs, new_fid, P9_DOTL_RDONLY | P9_DOTL_DIRECTORY);
	} else {
		r = fid_open(fs, new_fid, P9_DOTL_RDWR);
		if (r == -EACCES || r == -EROFS || r == -EPERM) {
			r = fid_open(fs, new_fid, P9_DOTL_RDONLY);
			rdonly = true;
		}
	}

	if (r != 0) {
		kdprintf("9pfs: failed to open fid for %s: %d\n", node->name,
		    r);
		fid_clunk(fs, new_fid);
		ke_mutex_exit(&node->open_fid_lock);
		return r;
	}

	node->paging_fid_rdonly = rdonly;
	atomic_store_explicit(&node->paging_fid, new_fid, memory_order_release);

	ke_mutex_exit(&node->open_fid_lock);

	return 0;
}

static int
ninep_open(vnode_t **vnp, int flags)
{
	vnode_t *vn = *vnp;
	struct ninep_node *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(vn);
	int r;

	if (vn->type != VREG)
		return 0;

	r = node_make_paging_fid(fs, node);
	if (r != 0)
		return r;

	if ((flags & O_ACCMODE) != O_RDONLY && node->paging_fid_rdonly)
		return -EACCES;

	return 0;
}

enum vtype
mode_to_vtype(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return VDIR;

	case S_IFCHR:
		return VCHR;

	case S_IFBLK:
		return VBLK;

	case S_IFREG:
		return VREG;

	case S_IFIFO:
		return VNON;

	case S_IFLNK:
		return VLNK;

	case S_IFSOCK:
		return VSOCK;

	default:
		return VNON;
	}
}

static int
do_getattr(struct ninepfs_state *fs, fid_t fid, vattr_t *vattr, bool want_gen)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	uint64_t valid;
	struct ninep_qid qid;
	uint64_t nlink, blocks, btime, gen_val;

	/* size[4] Tgetattr tag[2] fid[4] request_mask[8] */
	buf_in = ninep_buf_alloc("Fl");
	/*
	 * size[4] Rgetattr tag[2] valid[8] qid[13] mode[4] uid[4] gid[4]
	 *       nlink[8] rdev[8] size[8] blksize[8] blocks[8]
	 *       atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
	 *       ctime_sec[8] ctime_nsec[8] btime_sec[8] btime_nsec[8]
	 *       gen[8] data_version[8]
	 */
	buf_out = ninep_buf_alloc("lQdddlllllllllllllll");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pGetattr;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addu64(buf_in,
	    k9pGetattrBasic | (want_gen ? k9pGetattrGen : 0));
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pGetattr + 1:
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		return -err;
		break;
	}

	default:
		kfatal("Unexpected ninep result");
	}

	ninep_buf_getu64(buf_out, &valid);
	kassert((valid & k9pGetattrBasic) == k9pGetattrBasic);
	ninep_buf_getqid(buf_out, &qid);
	ninep_buf_getu32(buf_out, &vattr->mode);
	ninep_buf_getu32(buf_out, &vattr->uid);
	ninep_buf_getu32(buf_out, &vattr->gid);
	ninep_buf_getu64(buf_out, &nlink);
	ninep_buf_getu64(buf_out, &vattr->rdev);
	ninep_buf_getu64(buf_out, &vattr->size);
	ninep_buf_getu64(buf_out, &vattr->bsize);
	ninep_buf_getu64(buf_out, &blocks);
	ninep_buf_gettime(buf_out, &vattr->atim.tv_sec);
	ninep_buf_gettime(buf_out, &vattr->atim.tv_nsec);
	ninep_buf_gettime(buf_out, &vattr->mtim.tv_sec);
	ninep_buf_gettime(buf_out, &vattr->mtim.tv_nsec);
	ninep_buf_gettime(buf_out, &vattr->ctim.tv_sec);
	ninep_buf_gettime(buf_out, &vattr->ctim.tv_nsec);
	ninep_buf_getu64(buf_out, &btime); /* (btime_sec) */
	ninep_buf_getu64(buf_out, &btime); /* (btime_nsec) */
	ninep_buf_getu64(buf_out, &gen_val);

	vattr->gen = (valid & k9pGetattrGen) != 0 ? gen_val :
	    NOGEN;

	vattr->fileid = qid.path;
	vattr->fsid = 0;
	vattr->type = mode_to_vtype(vattr->mode);
	vattr->nlink = nlink;
	vattr->dsize = blocks * vattr->bsize;

	return 0;
}

static enum vn_idcache_decision
ninep_decide(vnode_t *vn, void *)
{
	struct ninep_node *node = VTO9(vn);

	/*
	 * Node was evicted (unlinked, renamed over, or stale) so nobody can
	 * ever reach it again and it can go away altogether.
	 */
	if (!node->idc_entry.inserted)
		return kVcacheInactiveProceed;

	if (vn->type == VREG && (viewcache_vnode_dirty(vn) ||
	    vm_obj_dirty_pages(vn->file.vmobj) != 0))
		return kVcacheInactiveRetain;

	/* no other metadata to deal with in 9p. */
	return kVcacheInactiveProceed;
}

static void
node_teardown(struct ninepfs_state *fs, struct ninep_node *node)
{
	vnode_t *vn = node->vnode;

	kassert(node != fs->root_node, "tearing down the 9p root");

	ke_rwlock_enter_write(&node->rwlock, "node_teardown");

	if (vn->type == VREG) {
		vm_object_t *obj = vn->file.vmobj;
		size_t discarded;

		/* wait for any writebacks to finish and clear off any views */
		viewcache_truncate(vn, 0);

		/* paging barrier */
		ke_rwlock_enter_write(&node->paging_rwlock, "node_teardown");
		vm_vnobj_set_valid_length(obj, 0);
		ke_rwlock_exit_write(&node->paging_rwlock);

		/* make away with every page, irrespective of vattr.size */
		discarded = vm_obj_purge(obj);
		vm_obj_drain_io(obj);

		/* only evicted nodes should have any data left to discard */
		kassert(discarded == 0 || !node->idc_entry.inserted,
		    "node_teardown: dirty pages were left in a clean vnode");
	}

	/* now that all paging I/O is done, the paging fid can go away */
	if (node->paging_fid != 0) {
		fid_clunk(fs, node->paging_fid);
		node->paging_fid = 0;
	}
	if (node->fid != NOFID) {
		fid_clunk(fs, node->fid);
		node->fid = NOFID;
	}

	ke_rwlock_exit_write(&node->rwlock);

	if (node->name != NULL)
		kmem_strfree(node->name);

	vn_idcache_inactive_end(&fs->idcache, &node->idc_entry);
	vn_free(vn);
	kmem_free(node, sizeof(*node));
	atomic_fetch_add_explicit(&fs->nodes_freed, 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&fs->nodes_torn_down, 1,
	    memory_order_relaxed);
}

static int
ninep_inactive(vnode_t *vn)
{
	struct ninep_node *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(vn);

	if (node == fs->root_node) {
		bool frozen;

		kassert(node->unlinked, "ninep_inactive: root before umount");
		frozen = vn_tryfreeze(vn);
		kassert(frozen, "ninep_inactive: root is retained");
		(void)frozen;
		fs->root_node = NULL;
		if (node->name != NULL)
			kmem_strfree(node->name);
		vn_free(vn);
		kmem_free(node, sizeof(*node));
		atomic_fetch_add_explicit(&fs->nodes_freed, 1,
		    memory_order_relaxed);
		return 0;
	}

	switch (vn_idcache_inactive_begin(&fs->idcache, &node->idc_entry,
	    ninep_decide, NULL)) {
	case kVcacheInactiveEAGAIN:
		return -EAGAIN;

	case kVcacheInactiveRetain:
		/* inactive_begin() took the writer's reference so send it */
		viewcache_request_sync(vn);
		return -EAGAIN;

	case kVcacheInactiveProceed:
		node_teardown(fs, node);
		return 0;
	}

	kunreachable();
}

int
ninep_lookup(vnode_t *dvn, const char *name, vnode_t **out)
{
	struct ninep_node *dn = VTO9(dvn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	fid_t new_fid, dir_fid;
	struct ninep_qid qid;
	struct ninep_node *node;
	int r;

	if (dn->unlinked)
		return -ENOENT;

	r = node_walk_fid(fs, dn, &dir_fid);
	if (r != 0)
		return r;

	new_fid = fid_allocate(fs);

	r = fid_walk1(fs, dir_fid, new_fid, name, &qid);
	if (r != 0)
		return r;

	r = node_for_qid(fs, &node, qid, new_fid, VNON);
	if (r == 1) {
		bool used = false;

		ke_rwlock_enter_write(&node->rwlock, "node_adopt_walk_fid");
		if (node->fid == NOFID) {
			node->fid = new_fid;
			used = true;
		}
		ke_rwlock_exit_write(&node->rwlock);

		if (!used)
			fid_clunk(fs, new_fid);

	} else if (r == 0) {
		node->name = kmem_strdup(name);
	} else {
		fid_clunk(fs, new_fid);
		return r;
	}

	*out = node->vnode;

	return 0;
}

static int
ninep_mkdir(vnode_t *dvn, const char *name, vattr_t *attr, vnode_t **out)
{
	struct ninep_node *dn = VTO9(dvn), *node;
	struct ninepfs_state *fs = VTO9FS(dvn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	struct ninep_qid qid;
	uint32_t mode;
	fid_t walkfid, dir_fid;
	int r = 0;

	if (dn->unlinked)
		return -ENOENT;

	r = node_walk_fid(fs, dn, &dir_fid);
	if (r != 0)
		return r;

	mode = 0755;

	/* size[4] Tmkdir tag[2] dfid[4] name[s] mode[4] gid[4] */
	buf_in = ninep_buf_alloc("FS64dd");
	/* size[4] Rmkdir tag[2] qid[13] */
	buf_out = ninep_buf_alloc("Q");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pMkDir;
	ninep_buf_addfid(buf_in, dir_fid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in, mode);
	ninep_buf_addu32(buf_in, 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pMkDir + 1: {
		ninep_buf_getqid(buf_out, &qid);
		ninep_buf_free(buf_out);
		r = 0;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		kdprintf("9p: Tmkdir failed: %d\n", r);
		goto out;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	walkfid = fid_allocate(fs);

	r = fid_walk1(fs, dir_fid, walkfid, name, &qid);
	if (r != 0)
		goto out;

	r = node_for_new_qid(fs, &node, qid, walkfid, VNON);
	if (r < 0) {
		fid_clunk(fs, walkfid);
		goto out;
	} else if (r == 1) {
		fid_clunk(fs, walkfid);
	} else {
		node->name = kmem_strdup(name);
	}
	*out = node->vnode;
	r = 0;

out:
	return r;
}

int
ninep_create(vnode_t *dvn, const char *name, vattr_t *attr, vnode_t **out)
{
	struct ninep_node *dn = VTO9(dvn), *res;
	struct ninepfs_state *fs = VTO9FS(dvn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	ninep_fid_t newfid, walkfid, dir_fid;
	struct ninep_qid qid;
	int nineplmode = 0;
	bool keep_create_fid;
	int r = 0;

	if (dn->unlinked)
		return -ENOENT;

	switch (attr->type) {
	case VREG:
		nineplmode = (attr->mode & 07777) | S_IFREG;
		break;

	case VSOCK:
		nineplmode = (attr->mode & 07777) | S_IFSOCK;
		break;

	case VDIR:
		return ninep_mkdir(dvn, name, attr, out);

	default:
		kfatal("Unexpected vattr type %d\n", attr->type);
	}

	/*
	 * Tlcreate replaces the directory FID with an open FID for the new file
	 * so clone the FID directory.
	 *
	 * and for regular-files with no owner-write permission keep the
	 * writable FID from create, otherwise reopening it later can fail.
	 * Otherwise it can go and separate FID opened as needed.
	 */
	keep_create_fid = attr->type == VREG && (nineplmode & S_IWUSR) == 0;

	r = node_walk_fid(fs, dn, &dir_fid);
	if (r != 0)
		return r;

	newfid = fid_allocate(fs);
	r = fid_clone(fs, dir_fid, newfid);
	if (r != 0)
		return r;

	/* size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4] */
	buf_in = ninep_buf_alloc("FS64ddd");
	/* size[4] Rlcreate tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLcreate;
	ninep_buf_addfid(buf_in, newfid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in,
	    P9_DOTL_RDWR | P9_DOTL_CREATE | P9_DOTL_EXCL); /* flags */
	ninep_buf_addu32(buf_in, nineplmode);
	ninep_buf_addu32(buf_in, 0); /* gid */
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pLcreate + 1: {
		ninep_buf_getqid(buf_out, &qid);
		ninep_buf_free(buf_out);
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		if (r != -EEXIST)
			kdprintf("9pfs: Tlcreate of %s failed: %d\n", name, r);
		/* creation failed, the FID is still unopened */
		fid_clunk(fs, newfid);
		return r;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	/* get an unopened FID for node->fid */
	walkfid = fid_allocate(fs);
	r = fid_walk1(fs, dir_fid, walkfid, name, NULL);
	if (r != 0) {
		kdprintf("9pfs: walk of just-created %s failed: %d\n", name, r);
		fid_clunk(fs, newfid);
		return r;
	}

	if (!keep_create_fid)
		fid_clunk(fs, newfid);

	/*
	 * Given Tlcreate with O_EXCL should make a fresh inode, make away with
	 * any cached node that somehow has the same identity.
	 */
	r = node_for_new_qid(fs, &res, qid, walkfid, attr->type);
	if (r < 0) {
		fid_clunk(fs, walkfid);
		if (keep_create_fid)
			fid_clunk(fs, newfid);
		return r;
	} else if (r == 1) {
		fid_clunk(fs, walkfid);
		if (keep_create_fid)
			fid_clunk(fs, newfid);
	} else {
		res->name = kmem_strdup(name);
		if (keep_create_fid) {
			/*
			 * The vnode having been published, a concurrent holder
			 * might have opened an FID, so get rid of ours if so.
			 */
			ke_mutex_enter(&res->open_fid_lock, "ninep_create");
			if (res->paging_fid == 0) {
				res->paging_fid = newfid;
				res->paging_fid_rdonly = false;
				newfid = 0;
			}
			ke_mutex_exit(&res->open_fid_lock);
			if (newfid != 0)
				fid_clunk(fs, newfid);
		}
	}

	*out = res->vnode;

	return 0;
}

int
ninep_link(vnode_t *dvn, vnode_t *vn, const char *name)
{
	struct ninep_node *dn = VTO9(dvn), *n = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	fid_t dir_fid, target_fid;
	int r = 0;

	kassert(dvn->vfs->fsprivate_1 == vn->vfs->fsprivate_1);

	if (dn->unlinked)
		return -ENOENT;

	r = node_walk_fid(fs, dn, &dir_fid);
	if (r != 0)
		return r;

	/* target fid goes stale if the name it was walked through is gone */
	r = node_walk_fid(fs, n, &target_fid);
	if (r != 0)
		return r;

	/* size[4] Tlink tag[2] dfid[4] fid[4] name[s] */
	buf_in = ninep_buf_alloc("FFS64");
	/* size[4] Rlink tag[2] */
	buf_out = ninep_buf_alloc("S64");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLink;
	ninep_buf_addfid(buf_in, dir_fid);
	ninep_buf_addfid(buf_in, target_fid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_close(buf_in);

	/* serialise against unlink's check of the link count */
	ke_rwlock_enter_write(&n->rwlock, "ninep_link");
	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ke_rwlock_exit_write(&n->rwlock);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pLink + 1:
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		kassert(r != 0);
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

/*
 * We make sure there's a paging FID before removing a name of the given node
 * so that anyone with that vnode keeps working and so that the 9p server
 * doesn't reuse the inode before teardown.
 *
 * We check the link count so we know whether this removal would take the last
 * link (or if the node is a directory) and if so return true to indicate that
 * the node must be evicted on success.
 *
 * The node rwlock stays held and must be dropped by the caller with
 * node_remove_end().
 */
static bool
node_remove_begin(struct ninepfs_state *fs, struct ninep_node *node)
{
	vattr_t attr;
	fid_t fid;
	int r;

	if (node->vnode->type == VREG || node->vnode->type == VDIR) {
		r = node_make_paging_fid(fs, node);
		if (r != 0)
			kdprintf("9pfs: can't pin %s before removal: %d\n",
			    node->name, r);
	}

	ke_rwlock_enter_write(&node->rwlock, "node_remove_begin");

	if (node->vnode->type == VDIR)
		return true;

	fid = node->paging_fid;
	if (fid == 0)
		r = node_walk_fid(fs, node, &fid);
	else
		r = 0;
	if (r == 0)
		r = do_getattr(fs, fid, &attr, false);
	if (r != 0) {
		kdprintf("9pfs: getattr of %s before removal failed: %d; "
		    "evicting\n", node->name, r);
		return true;
	}

	return attr.nlink <= 1;
}

static void
node_remove_end(struct ninepfs_state *fs, struct ninep_node *node, bool evict,
    bool removed)
{
	if (removed) {
		if (evict)
			node_evict(fs, node);
		/* the walk fid was for a removed name, so probably stale now */
		if (node->fid != NOFID) {
			fid_clunk(fs, node->fid);
			node->fid = NOFID;
		}
	}
	ke_rwlock_exit_write(&node->rwlock);
}

static int
ninep_unlinkat(vnode_t *dvn, const char *name, vnode_t *vn, bool removedir)
{
	struct ninep_node *dn = VTO9(dvn), *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	fid_t dir_fid;
	bool evict;
	int r;

	if (dn->unlinked)
		return -ENOENT;

	r = node_walk_fid(fs, dn, &dir_fid);
	if (r != 0)
		return r;

	evict = node_remove_begin(fs, node);

	/* size[4] Tunlinkat tag[2] dirfd[4] name[s] flags[4] */
	buf_in = ninep_buf_alloc("FS64d");
	/* size[4] Runlinkat tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pUnlinkAt;
	ninep_buf_addfid(buf_in, dir_fid); /* dirfids don't have to be open */
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in, removedir ? P9_DOTL_AT_REMOVEDIR : 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pUnlinkAt + 1:
		r = 0;
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		kassert(r != 0);
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	node_remove_end(fs, node, evict, r == 0);

	return r;
}

int
ninep_remove(vnode_t *dvn, const char *name, vnode_t *vn)
{
	return ninep_unlinkat(dvn, name, vn, false);
}

int
ninep_rmdir(vnode_t *dvn, const char *name, vnode_t *vn)
{
	return ninep_unlinkat(dvn, name, vn, true);
}

int
ninep_rename(vnode_t *old_dvn, const char *old_name, vnode_t *svn,
    vnode_t *new_dvn, const char *new_name, vnode_t *tvn)
{
	struct ninepfs_state *self = VTO9FS(old_dvn);
	struct ninep_node *old_dirnode = VTO9(old_dvn),
			  *new_dirnode = VTO9(new_dvn),
			  *snode = VTO9(svn),
			  *tnode = tvn != NULL ? VTO9(tvn) : NULL;
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	fid_t old_dir_fid, new_dir_fid;
	bool evict_target = false;
	int r = 0;

	if (old_dirnode->unlinked || new_dirnode->unlinked)
		return -ENOENT;

	r = node_walk_fid(self, old_dirnode, &old_dir_fid);
	if (r != 0)
		return r;
	r = node_walk_fid(self, new_dirnode, &new_dir_fid);
	if (r != 0)
		return r;

	/* give the displaced node the same treatment as if unlinked */
	if (tnode != NULL)
		evict_target = node_remove_begin(self, tnode);

	/*
	 * size[4] Trenameat tag[2] olddirfid[4] oldname[s] newdirfid[4]
	 *   newname[s]
	 */
	buf_in = ninep_buf_alloc("FS64FS64");
	/* size[4] Rlink tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(self->req_tag++);
	buf_in->data->kind = k9pRenameAt;
	ninep_buf_addfid(buf_in, old_dir_fid);
	ninep_buf_addstr(buf_in, old_name);
	ninep_buf_addfid(buf_in, new_dir_fid);
	ninep_buf_addstr(buf_in, new_name);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(self->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pRenameAt + 1:
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		kassert(r != 0);
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	if (tnode != NULL)
		node_remove_end(self, tnode, evict_target, r == 0);

	if (r == 0) {
		/* source keeps its walk FID - server changes the path */
		ke_rwlock_enter_write(&snode->rwlock, "ninep_rename");
		if (snode->name != NULL)
			kmem_strfree(snode->name);
		snode->name = kmem_strdup(new_name);
		ke_rwlock_exit_write(&snode->rwlock);
	}

	return r;
}

int
ninep_getattr(vnode_t *vn, vattr_t *attr)
{
	struct ninep_node *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(vn);
	vattr_t fresh;
	fid_t fid;
	int r;

	/* open FID is preferable as, with qemu, it still works after unlink  */
	fid = atomic_load_explicit(&node->paging_fid, memory_order_acquire);
	if (fid == 0)
		r = node_walk_fid(fs, node, &fid);
	else
		r = 0;
	if (r == 0)
		r = do_getattr(fs, fid, &fresh, false);

	/*
	 * Take the rwlock so the size can't race with extension by a write,
	 * which updates it under the rwlock as writer.
	 */
	ke_rwlock_enter_read(&node->rwlock, "ninep_getattr");
	if (r != 0) {
		/* FIXME: reasonable for dangling walk FID? other errors? */
		*attr = node->vattr;
	} else {
		/* not-yet-written-back writes might have extended it already */
		fresh.size = node->vattr.size;
		/* since e.g. sockets override the type, keep ours */
		fresh.type = node->vattr.type;
		/* gen wasn't requested, keep the one we know */
		fresh.gen = node->gen;
		node->vattr = fresh;
		*attr = fresh;
	}
	ke_rwlock_exit_read(&node->rwlock);

	return 0;
}

static int
ninep_do_setattr_size(struct ninepfs_state *fs, fid_t fid, uint64_t new_size)
{
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r = 0;

	/*
	 * size[4] Tsetattr tag[2] fid[4] valid[4] mode[4] uid[4] gid[4] size[8]
	 *           atime_sec[8] atime_nsec[8] mtime_sec[8] mtime_nsec[8]
	 */
	buf_in = ninep_buf_alloc("Fddddlllll");
	/* size[4] Rsetattr tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pSetattr;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addu32(buf_in, P9_SETATTR_SIZE);
	ninep_buf_addu32(buf_in, 0);		/* mode (ignored) */
	ninep_buf_addu32(buf_in, 0);		/* uid (ignored) */
	ninep_buf_addu32(buf_in, 0);		/* gid (ignored) */
	ninep_buf_addu64(buf_in, new_size);	/* size */
	ninep_buf_addu64(buf_in, 0);		/* atime_sec (ignored) */
	ninep_buf_addu64(buf_in, 0);		/* atime_nsec (ignored) */
	ninep_buf_addu64(buf_in, 0);		/* mtime_sec (ignored) */
	ninep_buf_addu64(buf_in, 0);		/* mtime_nsec (ignored) */
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pSetattr + 1:
		r = 0;
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		break;
	}

	default:
		kfatal("9p: unexpected Tsetattr response\n");
	}

	ninep_buf_free(buf_out);
	return r;
}

static int
ninep_do_fsync(struct ninepfs_state *fs, fid_t fid, bool datasync)
{
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r = 0;

	/* size[4] Tfsync tag[2] fid[4] datasync[4] */
	buf_in = ninep_buf_alloc("Fd");
	/* size[4] Rfsync tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pFsync;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addu32(buf_in, datasync ? 1 : 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pFsync + 1:
		r = 0;
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		break;
	}

	default:
		kfatal("9p: unexpected Tfsync response\n");
	}

	ninep_buf_free(buf_out);
	return r;
}


static int
ninep_fsync(vnode_t *vn, int flags)
{
	struct ninep_node *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(vn);
	int r;

	if (vn->type != VREG && vn->type != VDIR)
		return 0;

	if (vn->type == VREG) {
		/* protect against truncation */
		ke_rwlock_enter_read(&node->rwlock, "ninep_fsync");
		/* collecting before viewcache sync avoids some extra writes */
		vm_obj_collect_dirty_mappings(vn->file.vmobj);
		viewcache_sync_vnode(vn);
		vm_obj_flush(vn->file.vmobj);
		ke_rwlock_exit_read(&node->rwlock);
	}

	if (!(flags & VN_FSYNC_DURABLE))
		return 0;

	r = node_make_paging_fid(fs, node);
	if (r != 0)
		return r;

	return ninep_do_fsync(fs, node->paging_fid,
	    (flags & VN_FSYNC_DATAONLY) != 0);
}

static int
ninep_truncate(vnode_t *vn, size_t size)
{
	struct ninep_node *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(vn);
	int r;

	if (vn->type != VREG)
		return -EINVAL;

	r = node_make_paging_fid(fs, node);
	if (r != 0)
		return r;
	if (node->paging_fid_rdonly)
		return -EACCES;

	/* blocks all read/write calls, blocking all viewcache io */
	ke_rwlock_enter_write(&node->rwlock, "9p truncate");

	if (size < node->vattr.size)
		viewcache_truncate(vn, size);

	/* blocks paging io */
	ke_rwlock_enter_write(&node->paging_rwlock, "9p truncate");
	/* faults will no longer create busy pages beyond new size */
	vm_vnobj_set_valid_length(vn->file.vmobj, size);
	/* pageouts can proceed */
	ke_rwlock_exit_write(&node->paging_rwlock);

	if (size < node->vattr.size)
		vm_obj_truncate(vn->file.vmobj, node->vattr.size, size);

	/*
	 * TODO: for extend (rather than  true truncate) maybe zero any partial
	 * page that exists? could have junk in it from shared mappings.
	 */

	r = ninep_do_setattr_size(fs, node->paging_fid, size);
	if (r == 0) {
		node->vattr.size = size;
	} else {
		kdprintf("9pfs: setattr size failed!!");

		/* best-effort ""recovery"" "*/
		ke_rwlock_enter_write(&node->paging_rwlock, "9p truncate");
		vm_vnobj_set_valid_length(vn->file.vmobj, node->vattr.size);
		ke_rwlock_exit_write(&node->paging_rwlock);
	}

	ke_rwlock_exit_write(&node->rwlock);
	return 0;
}

int
ninep_setattr(vnode_t *vn, vattr_t *attr)
{
	int r = 0;

	if (attr->size != -1) {
		r = ninep_truncate(vn, attr->size);
	}

	return r;
}

static inline size_t
DIRENT_RECLEN(size_t namelen)
{
	size_t base = offsetof(struct dirent, d_name);
	size_t n = base + namelen + 1; /* include NUL */
	size_t a = sizeof(long);
	return (n + (a - 1)) & ~(a - 1);
}

static int
ninep_readdir(vnode_t *dvn, void *buf, size_t buflen, off_t *offset)
{
	struct ninep_node *node = VTO9(dvn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	off_t r = 0;
	char *buf_limit = (char*)buf + buflen;

	if (node->unlinked)
		return -ENOENT;

	r = node_make_paging_fid(fs, node);
	if (r != 0)
		return r;

	/* size[4] Treaddir tag[2] fid[4] offset[8] count[4] */
	buf_in = ninep_buf_alloc("Fld");
	/* size[4] Rreaddir tag[2] count[4] data[count] */
	buf_out = ninep_buf_alloc_bytes(buflen + 4);

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pReaddir;
	ninep_buf_addfid(buf_in, node->paging_fid);
	ninep_buf_addu64(buf_in, *offset);
	ninep_buf_addu32(buf_in, buflen);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pReaddir + 1:
		break;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		kassert(r != 0);
		goto out;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	uint32_t bytes_from_9p;
	size_t bytes_copied_out;
	off_t last_offset;

	bytes_copied_out = 0;
	last_offset = *offset;

	r = ninep_buf_getu32(buf_out, &bytes_from_9p);
	kassert(r == 0);

	while (bytes_from_9p) {
		struct ninep_qid qid;
		uint64_t offset;
		uint8_t type;
		char *name;
		struct dirent *out_buf = (struct dirent *)buf;

		r = ninep_buf_getqid(buf_out, &qid);
		if (r != 0) {
			break;
		}

		r = ninep_buf_getu64(buf_out, &offset);
		kassert(r == 0);

		r = ninep_buf_getu8(buf_out, &type);
		kassert(r == 0);

		/* TODO: eliminate double copy */
		r = ninep_buf_getstr(buf_out, &name);
		kassert(r == 0);

		/* make sure it can fit */
		if ((char*)buf + DIRENT_RECLEN(strlen(name)) > buf_limit) {
			kmem_free(name, strlen(name) + 1);
			break;
		}

		out_buf->d_off = offset;
		out_buf->d_ino = qid.path;
		out_buf->d_type = type;
		strcpy(out_buf->d_name, name);
		out_buf->d_reclen = DIRENT_RECLEN(strlen(name));

		kmem_free(name, strlen(name) + 1);
		last_offset = offset;
		*(char **)&buf += out_buf->d_reclen;
		bytes_copied_out += out_buf->d_reclen;
	}

	ninep_buf_free(buf_out);

	r = bytes_copied_out;
	*offset = last_offset;

out:
	return r;
}

static int
ninep_readlink(vnode_t *vn, char *buf, size_t buflen)
{
	struct ninep_node *node = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(vn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	fid_t fid;
	off_t r = 0;

	r = node_walk_fid(fs, node, &fid);
	if (r != 0)
		return r;

	/* size[4] Treadlink tag[2] fid[4] */
	buf_in = ninep_buf_alloc("F");
	/* size[4] Rreadlink tag[2] target[s] */
	buf_out = ninep_buf_alloc("S80");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pReadlink;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pReadlink + 1: {
		char *str;
		ninep_buf_getstr(buf_out, &str);
		strncpy(buf, str, buflen);
		r = strlen(str);
		kmem_free(str, strlen(str) + 1);
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		r = -err;
		kassert(r != 0);
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

static int negotiate_version(struct ninepfs_state *state)
{
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r = 0;

	buf_in = ninep_buf_alloc("dS8");
	buf_out = ninep_buf_alloc("dS16");

	buf_in->data->tag = to_leu16(-1);
	buf_in->data->kind = k9pVersion;
	ninep_buf_addu32(buf_in, 4096 * 16 + 128);
	ninep_buf_addstr(buf_in, k9pVersion2000L);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(state->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);

	switch (buf_out->data->kind) {
	case k9pVersion + 1: {
		char *ver;
		uint32_t msize;

		ninep_buf_getu32(buf_out, &msize);
		ninep_buf_getstr(buf_out, &ver);

		kdprintf("9pfs: Negotiated 9p version %s, message size %d\n",
		    ver, msize);
		break;
	}

	default: {
		kdprintf("9pfs: Bad 9p version.\n");
		r = -1;
	}
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);
	iop_free(iop);

	return r;
}

int
attach(struct ninepfs_state *state)
{
	int r = 0;
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;

	/* size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4] */
	buf_in = ninep_buf_alloc("FFS4S45d");
	/* size[4] Rattach tag[2] qid[13] */
	buf_out = ninep_buf_alloc("Q");

	buf_in->data->tag = to_leu16(state->req_tag++);
	buf_in->data->kind = k9pAttach;
	ninep_buf_addfid(buf_in, fid_allocate(state));
	ninep_buf_addfid(buf_in, ~0);
	ninep_buf_addstr(buf_in, "root");
	ninep_buf_addstr(buf_in,
	    "/ws/Projects/Keyronex/build/amd64/system-root");
	ninep_buf_addu32(buf_in, 0);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(state->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);

	switch (buf_out->data->kind) {
	case k9pAttach + 1: {
		struct ninep_qid qid;

		r = ninep_buf_getqid(buf_out, &qid);
		if (r != 0)
			kfatal("Couldn't get a QID!\n");

		r = node_for_qid(state, &state->root_node, qid, 1, VNON);
		kassert(r == 0); /* can't be existing already */

		kdprintf("Attached, root FID type %d ver %d path %" PRIu64 "\n",
		    qid.type, qid.version, qid.path);

		break;
	}

	default: {
		kfatal("9p failure\n");
	}
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

static void
ninep_unmount(vfs_t *vfs)
{
	struct ninepfs_state *fs = (struct ninepfs_state *)vfs->fsprivate_1;
	struct ninep_node *root = fs->root_node;

	/*
	 * By now every vnode except the root is gone, so remove it from the
	 * idcache, clunk its FIDs, and then the root's inactive will just be
	 * a free.
	 */

	node_evict(fs, root);

	ke_rwlock_enter_write(&root->rwlock, "ninep_unmount");
	if (root->paging_fid != 0) {
		fid_clunk(fs, root->paging_fid);
		root->paging_fid = 0;
	}
	if (root->fid != NOFID) {
		fid_clunk(fs, root->fid);
		root->fid = NOFID;
	}
	ke_rwlock_exit_write(&root->rwlock);
}

static void
ninep_freevfs(vfs_t *vfs)
{
	struct ninepfs_state *fs = (struct ninepfs_state *)vfs->fsprivate_1;

	kassert(fs->root_node == NULL, "ninep_free: root still exists");
	kdprintf("9p: unmounted: fids alloc=%u clunked=%u unused=%u "
	    "nodes created=%u freed=%u\n",
	    atomic_load_explicit(&fs->fids_allocated, memory_order_relaxed),
	    atomic_load_explicit(&fs->fids_clunked, memory_order_relaxed),
	    atomic_load_explicit(&fs->fids_unused, memory_order_relaxed),
	    atomic_load_explicit(&fs->nodes_created, memory_order_relaxed),
	    atomic_load_explicit(&fs->nodes_freed, memory_order_relaxed));
	vn_idcache_fini(&fs->idcache);
	vn_release(fs->provider);
	if (ninep_debug_state == fs)
		ninep_debug_state = NULL;
	kmem_free(fs, sizeof(*fs));
}

static const struct vfs_ops ninep_vfs_ops = {
	.unmount = ninep_unmount,
	.freevfs = ninep_freevfs,
};

static struct ninepfs_state *
ninep_attach(vnode_t *provider, vfs_t *vfs)
{
	struct ninepfs_state *state = kmem_alloc(sizeof(struct ninepfs_state));
	int r;

	atomic_store(&state->fid_counter, 1);
	atomic_store(&state->req_tag, 0);
	atomic_store(&state->fids_allocated, 0);
	atomic_store(&state->fids_clunked, 0);
	atomic_store(&state->fids_unused, 0);
	atomic_store(&state->nodes_created, 0);
	atomic_store(&state->nodes_freed, 0);
	atomic_store(&state->nodes_torn_down, 0);
	state->provider = provider;
	vn_idcache_init(&state->idcache, 1024);
	state->vfs = vfs;

	vfs_init(vfs);
	vfs->ops = &ninep_vfs_ops;
	vfs->fsprivate_1 = (uintptr_t)state;

	r = negotiate_version(state);
	if (r != 0)
		kfatal("ninep_mount: negotiate_version failed\n");

	r = attach(state);
	if (r != 0)
		kfatal("ninep_mount: attach failed\n");

	return state;
}

void
ninep_mountroot(vnode_t *provider)
{
	vfs_t *vfs = kmem_alloc(sizeof(vfs_t));
	struct ninepfs_state *state = ninep_attach(provider, vfs);

	ninep_debug_state = state;
	nc_mount(NCH_NULL, vfs, state->root_node->vnode);
}

int
ninep_mount(vnode_t *provider, namecache_handle_t overnch)
{
	vfs_t *vfs = kmem_alloc(sizeof(vfs_t));
	struct ninepfs_state *state = ninep_attach(provider, vfs);

	nc_mount(overnch, vfs, state->root_node->vnode);
	return 0;
}

static void
iop_frame_setup_9p(iop_frame_t *frame, vnode_t *provider,
    struct ninep_buf *ninep_in, struct ninep_buf *ninep_out, sg_list_t *mdl)
{
	frame->op = kIOP9p;
	frame->vp = provider;
	frame->ninep.ninep_in = ninep_in;
	frame->ninep.ninep_out = ninep_out;
	frame->sglist = mdl;
	frame->sglist_offset = 0;
}

iop_return_t
ninep_dispatch_iop(vnode_t *, iop_t *iop)
{
	iop_frame_t *frame = iop_current_frame(iop), *next_frame;
	struct ninep_node *node;
	struct ninep_buf *buf_in, *buf_out;
	struct ninepfs_state *m_state = VTO9FS(frame->vp);

	kassert(frame->op == kIOPRead || frame->op == kIOPWrite);

	node = VTO9(frame->vp);

	/* must be a read-hold on node->rwlock */

#if 1 /* FIXME: needs to be under appropriate lock */
	if (frame->rw.offset + frame->rw.length > node->vattr.size) {
		if (frame->rw.offset >= node->vattr.size)
			return kIOPRetCompleted;
		frame->rw.length = node->vattr.size - frame->rw.offset;
	}
#endif

	/* paging FID may not exist yet, e.g. exec */
	if (atomic_load_explicit(&node->paging_fid, memory_order_acquire) ==
	    0) {
		int r = node_make_paging_fid(m_state, node);
		if (r != 0)
			kfatal("9pfs: (%s) failed to make paging FID: %d\n",
			    node->name, r);
	}

	/*
	 * TODO: as well as not panicking, is it ever worth re-checking if this
	 * can be opened read-write?
	 */
	if (frame->op == kIOPWrite && node->paging_fid_rdonly)
		kfatal("9pfs: (%s) pager write through a read-only fid\n",
		    node->name);

	/* size[4] Tread tag[2] fid[4] offset[8] count[4] */
	buf_in = ninep_buf_alloc("Fld");
	/* size[4] Rread tag[2] count[4] data[count] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(m_state->req_tag++);
	buf_in->data->kind = frame->op == kIOPRead ? k9pRead : k9pWrite;
	ninep_buf_addfid(buf_in, node->paging_fid);
	ninep_buf_addu64(buf_in, frame->rw.offset);
	ninep_buf_addu32(buf_in, frame->rw.length);
	ninep_buf_close(buf_in);

	next_frame = iop_next_frame(iop);
	iop_frame_setup_9p(next_frame, m_state->provider, buf_in, buf_out,
	    frame->sglist);
	next_frame->sglist_write = frame->op == kIOPRead;

	return kIOPRetContinue;
}

iop_return_t
ninep_complete_iop(vnode_t *, iop_t *iop)
{
	iop_frame_t *frame = iop_next_frame(iop);

	kassert(frame->op == kIOP9p);
	switch (frame->ninep.ninep_out->data->kind) {
	case k9pRead + 1:
	case k9pWrite + 1: {
		uint32_t count;

		ninep_buf_getu32(frame->ninep.ninep_out, &count);
		kassert(count <= iop_current_frame(iop)->rw.length);

		ninep_buf_free(frame->ninep.ninep_in);
		ninep_buf_free(frame->ninep.ninep_out);

		iop->result = count;

		return kIOPRetCompleted;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(frame->ninep.ninep_out, &err);
		kdprintf("9pfs: Pager I/O got error code %d\n", err);
	}
	default:
		kfatal("9p error\n");
	}
}

static void
ninep_vc_enter(vnode_t *vn, bool write)
{
	struct ninep_node *node = VTO9(vn);
	ke_rwlock_enter_read(&node->rwlock, "ninep_vc_enter");
}

static void
ninep_vc_exit(vnode_t *vn, bool write)
{
	struct ninep_node *node = VTO9(vn);
	ke_rwlock_exit_read(&node->rwlock);
}

static void
ninep_paging_enter(vnode_t *vn)
{
	struct ninep_node *node = VTO9(vn);
	ke_rwlock_enter_read(&node->paging_rwlock, "ninep_paging_enter");
}

static void
ninep_paging_exit(vnode_t *vn)
{
	struct ninep_node *node = VTO9(vn);
	ke_rwlock_exit_read(&node->paging_rwlock);
}


static int
ninep_read(vnode_t *vn, void *buf, size_t buflen, off_t offset, int)
{
	struct ninep_node *node = VTO9(vn);
	int r;

	r = node_make_paging_fid(VTO9FS(vn), node);
	if (r != 0)
		return r;

	if (offset >= node->vattr.size)
		return 0;

	buflen = MIN2(buflen, node->vattr.size - offset);
	return viewcache_io(vn, offset, buflen, false, buf);
}

static int
ninep_write(vnode_t *vn, const void *buf, size_t buflen, off_t offset, int)
{
	struct ninep_node *node = VTO9(vn);
	int r;

	r = node_make_paging_fid(VTO9FS(vn), node);
	if (r != 0)
		return r;
	if (node->paging_fid_rdonly)
		return -EBADF;

	ke_rwlock_enter_write(&node->rwlock, "ninep_write");
	if (offset + buflen > node->vattr.size) {
		ke_rwlock_enter_read(&node->paging_rwlock,
		    "ninep_write:paging_rwlock");
		node->vattr.size = offset + buflen;
		vm_vnobj_set_valid_length(vn->file.vmobj, offset + buflen);
		ke_rwlock_exit_read(&node->paging_rwlock);
	}
	ke_rwlock_downgrade(&node->rwlock);
	r = viewcache_io(vn, offset, buflen, true, (void *)buf);
	ke_rwlock_exit_read(&node->rwlock);
	return r;
}

static int
ninep_seek(vnode_t *, off_t, off_t *)
{
	return 0;
}

static int
ninep_ioctl(vnode_t *vn, unsigned long cmd, void *arg)
{
	return -ENOTTY;
}

static struct vnode_ops ninep_vnops = {
	.open = ninep_open,
	.inactive = ninep_inactive,
	.lookup = ninep_lookup,
	.create = ninep_create,
	.link = ninep_link,
	.remove = ninep_remove,
	.rmdir = ninep_rmdir,
	.rename = ninep_rename,
	.getattr = ninep_getattr,
	.setattr = ninep_setattr,
	.fsync = ninep_fsync,
	.readdir = ninep_readdir,
	.readlink = ninep_readlink,
	.stack_depth = 2,
	.vc_enter = ninep_vc_enter,
	.vc_exit = ninep_vc_exit,
	.paging_enter = ninep_paging_enter,
	.paging_exit = ninep_paging_exit,
	.read = ninep_read,
	.write = ninep_write,
	.seek = ninep_seek,
	.ioctl = ninep_ioctl,
	.iop_dispatch = ninep_dispatch_iop,
	.iop_complete = ninep_complete_iop,
};
