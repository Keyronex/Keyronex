/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file 9p_vnops.c
 * @brief Vnode operations for 9p.
 */

/* FIXME: Old code, needs review!! */

#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/k_thread.h>
#include <sys/kmem.h>
#include <sys/krx_vfs.h>
#include <sys/stat.h>
#include <sys/tree.h>
#include <sys/vnode.h>

#include <dirent.h>
#include <inttypes.h>
#include <libkern/lib.h>

#include "9pbuf.h"

typedef uint32_t fid_t;

struct ninep_node {
	RB_ENTRY(ninep_node) rb_entry;
	struct ninep_qid qid;
	fid_t fid, paging_fid;
	vnode_t *vnode;
	krwlock_t rwlock, paging_rwlock;
	vattr_t vattr;

#if 1
	char *name;
#endif
};

struct ninepfs_state {
	vfs_t *vfs;
	uint16_t req_tag;
	fid_t fid_counter;
	RB_HEAD(ninep_node_rb, ninep_node) node_cache;
	kmutex_t node_cache_lock;
	struct ninep_node *root_node;
	struct vnode *provider;
};

#define VTO9(VNODE)   ((struct ninep_node *)(VNODE)->fsprivate_1)
#define VTO9FS(VNODE) ((struct ninepfs_state *)(VNODE)->vfs->fsprivate_1)

static struct vnode_ops ninep_vnops;

static int do_getattr(struct ninepfs_state *fs, fid_t fid, vattr_t *vattr);

static int64_t
node_cmp(struct ninep_node *x, struct ninep_node *y)
{
	return x->qid.path - y->qid.path;
}

RB_GENERATE(ninep_node_rb, ninep_node, rb_entry, node_cmp);

static fid_t
fid_allocate(struct ninepfs_state *fs)
{
	return fs->fid_counter++;
}

/*!
 * Finds the ninep_node corresponding to a QID if it exists in the cache, adding
 * a reference to its vnode if so, or, if not found, ceeates one.
 * @retval 1 if an extant node was found
 * @retval 0 if a node was created
 */
static int
node_for_qid(struct ninepfs_state *fs, struct ninep_node **out,
    struct ninep_qid qid, fid_t fid, vtype_t override_vtype)
{
	struct ninep_node key, *found;
#if 0
	vtype_t vtype;
#endif
	int r;

	key.qid = qid;

	ke_mutex_enter(&fs->node_cache_lock, "9p node_for_qid");

	found = RB_FIND(ninep_node_rb, &fs->node_cache, &key);
	if (found != NULL) {
		/* don't check version, it's just mtime... */
		kassert(found->qid.path == qid.path &&
		    found->qid.type == qid.type);
		vn_retain(found->vnode);
		ke_mutex_exit(&fs->node_cache_lock);
		*out = found;
		return 1;
	}

	found = kmem_alloc(sizeof(struct ninep_node));
	found->qid = qid;
	found->fid = fid;
	found->paging_fid = 0;
	RB_INSERT(ninep_node_rb, &fs->node_cache, found);
	ke_rwlock_init(&found->rwlock);
	ke_rwlock_init(&found->paging_rwlock);

	r = do_getattr(fs, fid, &found->vattr);

#if 0
	switch (qid.type) {
	case 0x80: /* QTDIR */
		vtype = VDIR;
		break;

	case 0x0: /* QTFILE */
		vtype = VREG;
		break;

	default:
		kfatal("Unhandled qid type %x\n", qid.type);
	}
#endif

	if (override_vtype != VNON)
		found->vattr.type = override_vtype;

	found->vnode = vn_alloc(fs->vfs, found->vattr.type, &ninep_vnops,
	    (uintptr_t)found, 0);

	ke_rwlock_enter_write(&found->rwlock, "");

	/* todo: mark the vnode bad so waiters can see... */
	if (r != 0)
		kfatal("node_for_qid: getattr fail\n");

	ke_rwlock_exit_write(&found->rwlock);
	ke_mutex_exit(&fs->node_cache_lock);

	*out = found;

	return 0;
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
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
}

static int
node_make_paging_fid(struct ninepfs_state *fs, struct ninep_node *node)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	ninep_fid_t new_fid;
	int r;

	if (node->paging_fid != 0)
		return 0;

	if (node->vnode->type != VREG && node->vnode->type != VDIR)
		return -1; /* EBADF */

	new_fid = fid_allocate(fs);

	r = fid_clone(fs, node->fid, new_fid);
	if (r != 0)
		kfatal("Failed to clone fid (%s) for paging: %d\n", node->name, r);

	/* size[4] Tlopen tag[2] fid[4] flags[4] */
	buf_in = ninep_buf_alloc("Fd");
	/* size[4] Rlopen tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLopen;
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu32(buf_in,
	    node->vnode->type == VDIR ? O_DIRECTORY : O_RDWR);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);

	ninep_buf_free(buf_in);

	switch (buf_out->data->kind) {
	case k9pLopen + 1: {
		r = 0;
		node->paging_fid = new_fid;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kdprintf("Failed to clone node id: %d\n", err);
		kassert(err != 0);
		r = -err;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_out);

	return r;
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
do_getattr(struct ninepfs_state *fs, fid_t fid, vattr_t *vattr)
{
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	uint64_t valid;
	struct ninep_qid qid;
	uint64_t nlink, blocks;

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
	ninep_buf_addu64(buf_in, k9pGetattrBasic);
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
		kdprintf(" * ninep: oddly, do_getattr failed with %d\n", err);
		return -err;
		break;
	}

	default:
		kfatal("Unexpected ninep result");
	}

	ninep_buf_getu64(buf_out, &valid);
	kassert(valid == k9pGetattrBasic);
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

	vattr->fileid = qid.path;
	vattr->fsid = 0;
	vattr->type = mode_to_vtype(vattr->mode);
	vattr->nlink = nlink;
	vattr->dsize = blocks * vattr->bsize;

	return 0;
}

static int
ninep_inactive(vnode_t *vn)
{
	/* todo... */
	atomic_fetch_add(&vn->refcount, 1);
	return 0;
}

int
ninep_lookup(vnode_t *dvn, const char *name, vnode_t **out)
{
	struct ninep_node *dn = VTO9(dvn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	fid_t new_fid = fid_allocate(fs);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r;

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFhS64");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("hQ");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, dn->fid);
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
		struct ninep_node *node;

		ninep_buf_getu16(buf_out, &nwqid);
		kassert(nwqid == 1);
		ninep_buf_getqid(buf_out, &qid);
		ninep_buf_free(buf_out);

		r = node_for_qid(fs, &node, qid, new_fid, VNON);
		if (r == 1) {
			/* todo: clunk the new_fid - don't need it!! */
			r = 0;
		} else if (r == 0) {
			node->name = kmem_strdup(name);
		} else if (r < 0) {
			kfatal("node_for_qid failed?\n");
		}

		*out = node->vnode;

		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		goto out;
	}

	default: {
		kfatal("9p error\n");
	}
	}

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
	ninep_fid_t newfid;
	struct ninep_qid qid;
	vattr_t vattr_out;
	int nineplmode = 0;
	int r = 0;

	/*
	 * note:
	 * "lcreate creates a regular file name in directory fid and prepares it
	 * for I/O."
	 *
	 * There doesn't appear to be a special mode for creating sockets, dirs,
	 * etc; accordingly we need to handle these.
	 */

	switch (attr->type) {
	case VREG:
		nineplmode = 0755 | S_IFREG;
		break;

	case VDIR:
		nineplmode = 0755 | S_IFDIR;
		break;

	case VSOCK:
		nineplmode = 0755 | S_IFSOCK;
		break;

	default:
		kfatal("Unexpected vattr type %d\n", attr->type);
	}

	newfid = fid_allocate(fs);
	r = fid_clone(fs, dn->fid, newfid);
	kassert(r == 0);

	/* size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4] */
	buf_in = ninep_buf_alloc("FS64ddd");
	/* size[4] Rlcreate tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLcreate;
	ninep_buf_addfid(buf_in, newfid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in, O_CREAT); /* flags */
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

		r = do_getattr(fs, newfid, &vattr_out);
		kassert(r == 0);
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		kdprintf("Create failed: %d\n", r);
		goto out;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	r = node_for_qid(fs, &res, qid, newfid, attr->type);
	if (r < 0) {
		kdprintf("oddly, node_for_qid failed with %d\n", r);
		return -r;
	} else if (r == 1) {
		kdprintf("9p: warning: lcreate for %s - exists??\n", name);
		r = 0;
	}

	*out = res->vnode;

out:
	return r;
}

int
ninep_link(vnode_t *dvn, vnode_t *vn, const char *name)
{
	struct ninep_node *dn = VTO9(dvn), *n = VTO9(vn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r = 0;

	kassert(dvn->vfs->fsprivate_1 == vn->vfs->fsprivate_1);

	/* size[4] Tlink tag[2] dfid[4] fid[4] name[s] */
	buf_in = ninep_buf_alloc("FFS64");
	/* size[4] Rlink tag[2] */
	buf_out = ninep_buf_alloc("S64");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pLink;
	ninep_buf_addfid(buf_in, dn->fid);
	ninep_buf_addfid(buf_in, n->fid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_close(buf_in);

	iop = iop_new_9p(fs->provider, buf_in, buf_out, NULL);
	iop_send_sync(iop);
	iop_free(iop);
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

int
ninep_remove(vnode_t *dvn, const char *name)
{
	struct ninep_node *node = VTO9(dvn);
	struct ninepfs_state *fs = VTO9FS(dvn);
	ninep_fid_t dirfid;
	struct ninep_buf *buf_in, *buf_out;
	iop_t *iop;
	int r;

	if (node->paging_fid == 0) {
		/*
		 * maybe we need two here to fix the 'reused after remove' bug?
		 * and clean up the last one only after we've hit inactive()
		 */
		int r = node_make_paging_fid(fs, node);
		kassert(r == 0);
	}
	dirfid = node->paging_fid;

	/* size[4] Tunlinkat tag[2] dirfd[4] name[s] flags[4] */
	buf_in = ninep_buf_alloc("FS64d");
	/* size[4] Runlinkat tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pUnlinkAt;
	ninep_buf_addfid(buf_in, dirfid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in, 0);
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

	return r;
}

int
ninep_rename(vnode_t *old_dvn, const char *old_name, vnode_t *new_dvn,
    const char *new_name)
{
	struct ninepfs_state *self = VTO9FS(old_dvn);
	struct ninep_node *old_dirnode = VTO9(old_dvn),
			  *new_dirnode = VTO9(new_dvn);
	iop_t *iop;
	struct ninep_buf *buf_in, *buf_out;
	int r = 0;

	/*
	 * size[4] Trenameat tag[2] olddirfid[4] oldname[s] newdirfid[4]
	 *   newname[s]
	 */
	buf_in = ninep_buf_alloc("FS64FS64");
	/* size[4] Rlink tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = to_leu16(self->req_tag++);
	buf_in->data->kind = k9pRenameAt;
	ninep_buf_addfid(buf_in, old_dirnode->fid);
	ninep_buf_addstr(buf_in, old_name);
	ninep_buf_addfid(buf_in, new_dirnode->fid);
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

	return r;
}

int
ninep_getattr(vnode_t *dvn, vattr_t *vattr)
{
	struct ninep_node *dn = VTO9(dvn);
#if 0
	struct ninepfs_state *fs = VTO9FS(dvn);
	return do_getattr(fs, dn->fid, vattr);
#else
	*vattr = dn->vattr;
	return 0;
#endif
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

	if (node->paging_fid == 0) {
		r = node_make_paging_fid(fs, node);
		if (r != 0) {
			kdprintf("9pfs: Failed to get a pager Fid! Error %ld\n", r);
			return r;
		}
	}

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
	off_t r = 0;

	/* size[4] Treadlink tag[2] fid[4] */
	buf_in = ninep_buf_alloc("F");
	/* size[4] Rreadlink tag[2] target[s] */
	buf_out = ninep_buf_alloc("S80");

	buf_in->data->tag = to_leu16(fs->req_tag++);
	buf_in->data->kind = k9pReadlink;
	ninep_buf_addfid(buf_in, node->fid);
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
	ninep_buf_addfid(buf_in, state->fid_counter++);
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

void
ninep_mount(vnode_t *provider)
{
	vfs_t *vfs = kmem_alloc(sizeof(vfs_t));
	struct ninepfs_state *state = kmem_alloc(sizeof(struct ninepfs_state));
	int r;

	state->fid_counter = 1;
	state->req_tag = 0;
	state->provider = provider;
	RB_INIT(&state->node_cache);
	ke_mutex_init(&state->node_cache_lock);
	state->vfs = vfs;

	vfs_init(vfs);
	vfs->fsprivate_1 = (uintptr_t)state;

	r = negotiate_version(state);
	if (r != 0)
		kfatal("ninep_mount: negotiate_version failed\n");

	r = attach(state);
	if (r != 0)
		kfatal("ninep_mount: attach failed\n");


	nc_makeroot(vfs, state->root_node->vnode);
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

#if 1 /* FIXME: needs to be under appropriate lock */
	if (frame->rw.offset + frame->rw.length > node->vattr.size) {
		if (frame->rw.offset >= node->vattr.size)
			return kIOPRetCompleted;
		frame->rw.length = node->vattr.size - frame->rw.offset;
	}
#endif

	if (node->paging_fid == 0) {
		int r = node_make_paging_fid(m_state, node);
		if (r != 0)
			kfatal("9pfs: (%s) failed to make paging FID: %d\n",
			    node->name, r);
	}

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
ninep_lock_for_vc_io(vnode_t *vn, bool write)
{
	struct ninep_node *node = VTO9(vn);
	ke_rwlock_enter_read(&node->rwlock, "ninep_lock_for_vc_io");
}

static void
ninep_unlock_for_vc_io(vnode_t *vn, bool write)
{
	struct ninep_node *node = VTO9(vn);
	ke_rwlock_exit_read(&node->rwlock);
}


static int
ninep_read(vnode_t *vn, void *buf, size_t buflen, off_t offset, int)
{
	struct ninep_node *node = VTO9(vn);
	buflen = MIN2(buflen, node->vattr.size - offset);
	return viewcache_io(vn, offset, buflen, false, buf);
}

static int
ninep_write(vnode_t *vn, const void *buf, size_t buflen, off_t offset, int)
{
	struct ninep_node *node = VTO9(vn);
	int r;
	ke_rwlock_enter_write(&node->rwlock, "ninep_write");
	if (offset + buflen > node->vattr.size)
		node->vattr.size = offset + buflen;
	/* could downgrade to a read lock here... */
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
	.inactive = ninep_inactive,
	.lookup = ninep_lookup,
	.create = ninep_create,
	.link = ninep_link,
	.remove = ninep_remove,
	.rename = ninep_rename,
	.getattr = ninep_getattr,
	.readdir = ninep_readdir,
	.readlink = ninep_readlink,
	.stack_depth = 2,
	.lock_for_vc_io = ninep_lock_for_vc_io,
	.unlock_for_vc_io = ninep_unlock_for_vc_io,
	.read = ninep_read,
	.write = ninep_write,
	.seek = ninep_seek,
	.ioctl = ninep_ioctl,
	.iop_dispatch = ninep_dispatch_iop,
	.iop_complete = ninep_complete_iop,
};
