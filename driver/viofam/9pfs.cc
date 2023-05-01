/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#include <sys/errno.h>
#include <sys/stat.h>

#include <dirent.h>

#include "9pfs_reg.h"
#include "abi-bits/fcntl.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/libkern.h"

#include "9pfs.hh"

/*! Reference-counted by their vnode. Their lifespans are equivalent */
struct ninepfs_node {
	/*! Entry in NinePFS::node_rbtree. */
	RB_ENTRY(ninepfs_node) rbt_entry;
	/*! Corresponding vnode. */
	vnode_t *vnode;

	bool has_generic_fid;

	/*! 9p Qid. ninep_qid::path is the unique identifier. */
	struct ninep_qid qid;
	/*! 9p main Fid. */
	ninep_fid_t fid;
	/*! 9p Fid for pager I/O or readdir() */
	ninep_fid_t generic_fid;
};

static int64_t
node_cmp(struct ninepfs_node *x, struct ninepfs_node *y)
{
	return x->qid.path - y->qid.path;
}

static uint64_t sequence_num = 0;

RB_PROTOTYPE(ninepfs_node_rbt, ninepfs_node, rbt_entry, node_cmp);
RB_GENERATE(ninepfs_node_rbt, ninepfs_node, rbt_entry, node_cmp);

NinePFS::NinePFS(device_t *provider, vfs_t *vfs)
    : vfs(vfs)
    , root_node(NULL)
{
	ke_spinlock_init(&fid_lock);
	ke_mutex_init(&nodecache_mutex);
	RB_INIT(&node_rbt);

	kmem_asprintf(&objhdr.name, "9pfs%d", sequence_num++);
	attach(provider);

	negotiateVersion();
	doAttach();

	/* todo: factor away into a mount operation */
	vfs->ops = &vfsops;
	vfs->data = (uintptr_t)this;
	vfs->dev = this;
}

io_9p_request *
NinePFS::new9pRequest(struct ninep_buf *buf_in, vm_mdl_t *mdl_in,
    struct ninep_buf *buf_out, vm_mdl_t *mdl_out)
{
	io_9p_request *req = new (kmem_general) io_9p_request;
	memset(req, 0x0, sizeof(*req));

	/*
	 * we explicitly don't adjust the in message to include the length of
	 * the mdl components (used for read/write, and readdir?)
	 * Qemu at least doesn't like it (writes utter junk to the file). Not
	 */

	req->ptr_in = buf_in;
	req->mdl_in = mdl_in;
	req->ptr_out = buf_out;
	req->mdl_out = mdl_out;
	req->pending = true;

	return req;
}

int
NinePFS::negotiateVersion()
{
	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;

	buf_in = ninep_buf_alloc("dS8");
	buf_out = ninep_buf_alloc("dS16");

	buf_in->data->tag = -1;
	buf_in->data->kind = k9pVersion;
	ninep_buf_addu32(buf_in, 8288);
	ninep_buf_addstr(buf_in, k9pVersion2000L);
	ninep_buf_close(buf_in);

	req = new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	switch (buf_out->data->kind) {
	case k9pVersion + 1: {
		char *ver;
		uint32_t msize;

		ninep_buf_getu32(buf_out, &msize);
		ninep_buf_getstr(buf_out, &ver);

		DKDevLog(this, "Negotiated 9p version %s, message size %d\n",
		    ver, msize);
		break;
	}

	default: {
		kfatal("9p failure\n");
	}
	}

	return 0;
}

int
NinePFS::doAttach()
{
	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;
	int r;

	/* size[4] Tattach tag[2] fid[4] afid[4] uname[s] aname[s] n_uname[4] */
	buf_in = ninep_buf_alloc("FFS4S4d");
	/* size[4] Rattach tag[2] qid[13] */
	buf_out = ninep_buf_alloc("Q");

	buf_in->data->tag = ninep_unique++;
	buf_in->data->kind = k9pAttach;
	ninep_buf_addfid(buf_in, fid_counter++);
	ninep_buf_addfid(buf_in, 0);
	ninep_buf_addstr(buf_in, "root");
	ninep_buf_addstr(buf_in, "root");
	ninep_buf_addu32(buf_in, 0);
	ninep_buf_close(buf_in);

	req = new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pAttach + 1: {
		struct ninep_qid qid;
		ninep_buf_getqid(buf_out, &qid);
		root_node = findOrCreateNodePair(VDIR, 0, &qid, 1);
		DKDevLog(this, "Attached, root FID type %d ver %d path %lu\n",
		    qid.type, qid.version, qid.path);
		r = 0;
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

ninepfs_node *
NinePFS::findOrCreateNodePair(vtype_t type, size_t size, struct ninep_qid *qid,
    int rdwrfid)
{
	struct ninepfs_node key, *node;
	kwaitstatus_t w;

	key.qid = *qid;
	w = ke_wait(&nodecache_mutex,
	    "FuseFS::findOrCreateNodePair nodecache_mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	node = RB_FIND(ninepfs_node_rbt, &node_rbt, &key);
	if (node != NULL) {
		/* don't check version, it's just mtime... */
		kassert(
		    node->qid.path == qid->path && node->qid.type == qid->type);
		obj_direct_retain(node->vnode);
		ke_mutex_release(&nodecache_mutex);
		return node;
	}

	/* This is just a lookup; we can't do any more. */
	if (type == VNON) {
		ke_mutex_release(&nodecache_mutex);
		return NULL;
	}

	node = new (kmem_general) ninepfs_node;

	node->qid = *qid;
	node->fid = rdwrfid;
	node->has_generic_fid = false;
	node->vnode = new (kmem_general) vnode;
	obj_initialise_header(&node->vnode->vmobj.objhdr, kObjTypeVNode);
	node->vnode->data = (uintptr_t)node;
	node->vnode->type = type;
	node->vnode->vfsp = vfs;
	node->vnode->ops = &vnops;
	node->vnode->vfsmountedhere = NULL;
	node->vnode->size = size;
	ke_mutex_init(&node->vnode->lock);

	node->vnode->vmobj.is_anonymous = false;
	ke_mutex_init(&node->vnode->vmobj.mutex);
	RB_INIT(&node->vnode->vmobj.page_rbtree);

	RB_INSERT(ninepfs_node_rbt, &node_rbt, node);

	ke_mutex_release(&nodecache_mutex);

	return node;
}

int
NinePFS::genericFid(ninepfs_node *node, ninep_fid_t &handle_out)
{
	if (node->has_generic_fid) {
		handle_out = node->generic_fid;
		return 0;
	}

	if (node->vnode->type != VREG && node->vnode->type != VDIR)
		return -EBADF;

	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;
	ninep_fid_t new_fid;
	int r;

	r = cloneNodeFID(node, new_fid);
	kassert(r == 0);

	/* size[4] Tlopen tag[2] fid[4] flags[4] */
	buf_in = ninep_buf_alloc("Fd");
	/* size[4] Rlopen tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = ninep_unique++;
	buf_in->data->kind = k9pLopen;
	ninep_buf_addfid(buf_in, new_fid);
	ninep_buf_addu32(buf_in,
	    node->vnode->type == VDIR ? O_DIRECTORY : O_RDWR);
	ninep_buf_close(buf_in);

	req = new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pLopen + 1: {
		r = 0;
		node->has_generic_fid = true;
		node->generic_fid = new_fid;
		handle_out = new_fid;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kdprintf("Failed to clone node id: %d\n", err);
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

void
NinePFS::allocFID(ninep_fid_t &out)
{
	ipl_t ipl = ke_spinlock_acquire(&fid_lock);
	out = fid_counter++;
	ke_spinlock_release(&fid_lock, ipl);
}

int
NinePFS::cloneNodeFID(ninepfs_node *node, ninep_fid_t &out)
{
	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;
	ninep_fid_t newfid;
	int r;

	allocFID(newfid);

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFh");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("h");

	buf_in->data->tag = ninep_unique++;
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, node->fid);
	ninep_buf_addfid(buf_in, newfid);
	ninep_buf_addu16(buf_in, 0);
	ninep_buf_close(buf_in);

	req = new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwnames;
		ninep_buf_getu16(buf_out, &nwnames);
		kassert(nwnames == 0);
		out = newfid;
		r = 0;
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		kdprintf("Failed to clone node id: %d\n", err);
	}

	default: {
		kfatal("9p error\n");
	}
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

/*
 * The realtype parameter lets the type of a regular file be overriden to a
 * socket (in the future, also a device, etc.)
 *
 * This permits sockets etc to be implemented. I couldn't see how to create a
 * socket with Lcreate. `mode` does not seem to affect the creation of the file
 * nor is it reflected in future Getattrs.
 */
int
NinePFS::doGetattr(vtype_t realtype, ninep_fid_t fid, vattr_t &vattr)
{
	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;
	int r;

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

	buf_in->data->tag = ninep_unique++;
	buf_in->data->kind = k9pGetattr;
	ninep_buf_addfid(buf_in, fid);
	ninep_buf_addu64(buf_in, k9pGetattrBasic);
	ninep_buf_close(buf_in);

	req = new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pGetattr + 1: {
		r = 0;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	struct ninep_qid qid;
	uint64_t valid;

	ninep_buf_getu64(buf_out, &valid);
	kassert(valid == k9pGetattrBasic);
	ninep_buf_getqid(buf_out, &qid);
	ninep_buf_getu32(buf_out, &vattr.mode);
	ninep_buf_getu32(buf_out, NULL); /* uid */
	ninep_buf_getu32(buf_out, NULL); /* gid */
	ninep_buf_getu64(buf_out, NULL); /* nlink */
	ninep_buf_getu64(buf_out, &vattr.rdev);
	ninep_buf_getu64(buf_out, &vattr.size);
	ninep_buf_getu64(buf_out, NULL); /* blksize */
	ninep_buf_getu64(buf_out, NULL); /* blocks */
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr.atim.tv_sec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr.atim.tv_nsec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr.mtim.tv_sec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr.mtim.tv_nsec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr.ctim.tv_sec);
	ninep_buf_getu64(buf_out, (uint64_t *)&vattr.ctim.tv_nsec);

	vattr.ino = qid.path;
	vattr.type = mode_to_vtype(vattr.mode);

	if (realtype == VSOCK) {
		kassert(vattr.type == VREG);
		vattr.type = VSOCK;
		vattr.mode &= ~S_IFREG;
		vattr.mode |= S_IFSOCK;
	}

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

int
NinePFS::create(vnode_t *vn, vnode_t **out, const char *name, vattr_t *attr)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data), *res;
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

	case VSOCK: {
		nineplmode = 0755 | S_IFSOCK;
		break;
	}

	default:
		kfatal("Unexpected vattr type %d\n", attr->type);
	}

	r = self->cloneNodeFID(node, newfid);
	kassert(r == 0);

	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;

	/* size[4] Tlcreate tag[2] fid[4] name[s] flags[4] mode[4] gid[4] */
	buf_in = ninep_buf_alloc("FS64ddd");
	/* size[4] Rlcreate tag[2] qid[13] iounit[4] */
	buf_out = ninep_buf_alloc("Qd");

	buf_in->data->tag = self->ninep_unique++;
	buf_in->data->kind = k9pLcreate;
	ninep_buf_addfid(buf_in, newfid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in, O_CREAT); /* flags */
	ninep_buf_addu32(buf_in, nineplmode);
	ninep_buf_addu32(buf_in, 0); /* gid */
	ninep_buf_close(buf_in);

	req = self->new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(self->provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pLcreate + 1: {
		ninep_buf_getqid(buf_out, &qid);

		ninep_buf_free(buf_in);
		ninep_buf_free(buf_out);

		r = self->doGetattr(attr->type, newfid, vattr_out);
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_in);
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

	res = self->findOrCreateNodePair(mode_to_vtype(vattr_out.mode),
	    vattr_out.size, &qid, newfid);
	if (!res)
		return -ENOENT;

	*out = res->vnode;

out:
	return r;
}

int
NinePFS::lookup(vnode_t *vn, vnode_t **out, const char *pathname)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data), *res;
	ninep_fid_t newfid;
	struct ninep_qid qid;
	vattr_t vattr;
	int r = 0;

#if DEBUG_9PFS == 1
	kdprintf("ninepfs_lookup(vnode: %p, fid: %u, \"%s\");\n", node->vnode,
	    node->fid, pathname);
#endif

	self->allocFID(newfid);

	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;

	/* size[4] Twalk tag[2] fid[4] newfid[4] nwname[2] nwname*(wname[s]) */
	buf_in = ninep_buf_alloc("FFhS64");
	/* size[4] Rwalk tag[2] nwqid[2] nwqid*(wqid[13]) */
	buf_out = ninep_buf_alloc("hQ");

	buf_in->data->tag = self->ninep_unique++;
	buf_in->data->kind = k9pWalk;
	ninep_buf_addfid(buf_in, node->fid);
	ninep_buf_addfid(buf_in, newfid);
	ninep_buf_addu16(buf_in, 1);
	ninep_buf_addstr(buf_in, pathname);
	ninep_buf_close(buf_in);

	req = self->new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(self->provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwnames;

		ninep_buf_getu16(buf_out, &nwnames);
		kassert(nwnames == 1);
		ninep_buf_getqid(buf_out, &qid);

		ninep_buf_free(buf_in);
		ninep_buf_free(buf_out);

		r = self->doGetattr(VNON, newfid, vattr);
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_in);
		ninep_buf_free(buf_out);
		r = -err;
		kassert(r != 0);
		goto out;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	/* TODO(high) !!! clunk the fid if we got an existing node.... */

	res = self->findOrCreateNodePair(mode_to_vtype(vattr.mode), vattr.size,
	    &qid, newfid);
	if (!res)
		return -ENOENT;

	*out = res->vnode;

out:
	return r;
}

int
NinePFS::getattr(vnode_t *vn, vattr_t *out)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data);
	return self->doGetattr(vn->type, node->fid, *out);
}

int
NinePFS::link(vnode_t *dvn, vnode_t *vn, const char *name)
{
	NinePFS *self = (NinePFS *)dvn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)dvn->data);
	ninepfs_node *oldnode = ((ninepfs_node *)vn->data);
	int r = 0;

	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;

	kassert(dvn->vfsp->data == vn->vfsp->data);

	/* size[4] Tlink tag[2] dfid[4] fid[4] name[s] */
	buf_in = ninep_buf_alloc("FFS64");
	/* size[4] Rlink tag[2] */
	buf_out = ninep_buf_alloc("S64");

	buf_in->data->tag = self->ninep_unique++;
	buf_in->data->kind = k9pLink;
	ninep_buf_addfid(buf_in, node->fid);
	ninep_buf_addfid(buf_in, oldnode->fid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_close(buf_in);

	req = self->new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(self->provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

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

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

int
NinePFS::read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_read(vn, buf, nbyte, off);
}

int
NinePFS::write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_write(vn, buf, nbyte, off);
}

int
NinePFS::readlink(vnode_t *vn, char *out)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data);
	int r = 0;

	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;

	/* size[4] Treadlink tag[2] fid[4] */
	buf_in = ninep_buf_alloc("F");
	/* size[4] Rreadlink tag[2] target[s] */
	buf_out = ninep_buf_alloc("S80");

	buf_in->data->tag = self->ninep_unique++;
	buf_in->data->kind = k9pReadlink;
	ninep_buf_addfid(buf_in, node->fid);
	ninep_buf_close(buf_in);

	req = self->new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(self->provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pReadlink + 1: {
		char *str;
		ninep_buf_getstr(buf_out, &str);
		strcpy(out, str);
		kmem_strfree(str);
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

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);

	return r;
}

int
NinePFS::remove(vnode_t *vn, const char *name)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data);
	ninep_fid_t dirfid;
	int r;

	r = self->genericFid(node, dirfid);
	if (r != 0) {
		DKDevLog(self, "Failed to get a pager Fid! Error %d\n", r);
		kfatal("Unhandled\n");
	}

	iop_t *iop;
	io_9p_request *req;
	ninep_buf *buf_in, *buf_out;

	/* size[4] Tunlinkat tag[2] dirfd[4] name[s] flags[4] */
	buf_in = ninep_buf_alloc("FS64d");
	/* size[4] Runlinkat tag[2] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = self->ninep_unique++;
	buf_in->data->kind = k9pUnlinkAt;
	ninep_buf_addfid(buf_in, dirfid);
	ninep_buf_addstr(buf_in, name);
	ninep_buf_addu32(buf_in, 0);
	ninep_buf_close(buf_in);

	req = self->new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(self->provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);

	switch (buf_out->data->kind) {
	case k9pUnlinkAt + 1:
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

off_t
NinePFS::readdir(vnode_t *vn, void *buf, size_t buf_size, size_t *bytes_read,
    off_t seqno)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data);
	off_t r = 0;

	iop_t *iop;
	io_9p_request *req;
	ninep_fid_t dirfid;
	ninep_buf *buf_in, *buf_out;

	kassert(buf_size <= 2048);

	r = self->genericFid(node, dirfid);
	if (r != 0) {
		DKDevLog(self, "Failed to get a pager Fid! Error %ld\n", r);
		kfatal("Unhandled\n");
	}

	/* size[4] Treaddir tag[2] fid[4] offset[8] count[4] */
	buf_in = ninep_buf_alloc("Fld");
	/* size[4] Rreaddir tag[2] count[4] data[count] */
	buf_out = ninep_buf_alloc_bytes(buf_size + 4);

	buf_in->data->tag = self->ninep_unique++;
	buf_in->data->kind = k9pReaddir;
	ninep_buf_addfid(buf_in, dirfid);
	ninep_buf_addu64(buf_in, seqno);
	ninep_buf_addu32(buf_in, buf_size);
	ninep_buf_close(buf_in);

	req = self->new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(self->provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);
	iop_free(iop);
	delete_kmem(req);
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
	last_offset = seqno;

	r = ninep_buf_getu32(buf_out, &bytes_from_9p);
	kassert(r == 0);

	/*
	 * we don't rangecheck our output buffer because the 9p dirent format is
	 * larger, so it will always fit. It might be prudent to assert this
	 * just in case.
	 */

	while (bytes_from_9p) {
		ninep_qid qid;
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

		out_buf->d_off = offset;
		out_buf->d_ino = qid.path;
		out_buf->d_type = type;
		strcpy(out_buf->d_name, name);
		out_buf->d_reclen = DIRENT_RECLEN(strlen(name));

		kmem_strfree(name);
		last_offset = offset;
		*(char **)&buf += out_buf->d_reclen;
		bytes_copied_out += out_buf->d_reclen;
	}

	ninep_buf_free(buf_out);

	r = last_offset;
	*bytes_read = bytes_copied_out;

out:
	return r;
}

int
NinePFS::root(vfs_t *vfs, vnode_t **vout)
{
	NinePFS *self = (NinePFS *)(vfs->data);
	obj_direct_retain(self->root_node->vnode);
	*vout = self->root_node->vnode;
	return 0;
}

iop_return_t
NinePFS::dispatchIOP(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop), *next_frame;
	io_9p_request *req;
	ninepfs_node *node = (ninepfs_node *)frame->vnode->data;
	ninep_fid_t myfid;
	ninep_buf *buf_in, *buf_out;
	bool iswrite = false;
	int r;

	iswrite = frame->function == kIOPTypeWrite;
	if (!iswrite)
		kassert(frame->function == kIOPTypeRead);

	/*
	 * The only IOPs for filesystems currently are pager in/out requests.
	 */

	next_frame = iop_stack_initialise_next(iop);

	r = genericFid(node, myfid);
	if (r != 0) {
		DKDevLog(this, "Failed to get a pager Fid! Error %d\n", r);
		return kIOPRetCompleted;
	}

	/* size[4] Tread tag[2] fid[4] offset[8] count[4] */
	buf_in = ninep_buf_alloc("Fld");
	/* size[4] Rread tag[2] count[4] data[count] */
	buf_out = ninep_buf_alloc("d");

	buf_in->data->tag = ninep_unique++;
	ninep_buf_addfid(buf_in, myfid);
	ninep_buf_addu64(buf_in, frame->rw.offset);
	ninep_buf_addu32(buf_in, frame->rw.bytes);
	ninep_buf_close(buf_in);

	if (iswrite) {
		buf_in->data->kind = k9pWrite;
	} else {
		buf_in->data->kind = k9pRead;
	}

	req = new9pRequest(buf_in, iswrite ? frame->mdl : NULL, buf_out,
	    iswrite ? NULL : frame->mdl);
	iop_frame_setup_ioctl(next_frame, kIOCTL9PEnqueueRequest, req,
	    sizeof(*req));
	req->iop = iop;

	return kIOPRetContinue;
}

iop_return_t
NinePFS::completeIOP(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_previous(iop);
	io_9p_request *req;

	kassert(frame->function == kIOPTypeIOCtl &&
	    frame->ioctl.type == kIOCTL9PEnqueueRequest);
	req = (io_9p_request *)frame->mdl;

	switch (req->ptr_out->data->kind) {
	case k9pRead + 1:
	case k9pWrite + 1:
		uint32_t count;

		ninep_buf_getu32(req->ptr_out, &count);
		// kassert(count == iop_stack_current(iop)->rw.bytes);

		ninep_buf_free(req->ptr_in);
		ninep_buf_free(req->ptr_out);
		delete_kmem(req);

		return kIOPRetCompleted;

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(req->ptr_out, &err);
		DKDevLog(this, "Pager I/O got error code %d\n", err);
	}
	default:
		kfatal("9p error\n");
	}
}

struct vfsops NinePFS::vfsops = {
	.root = root,
};

struct vnops NinePFS::vnops = {
	.read = read,
	.write = write,
	.getattr = getattr,

	.lookup = lookup,
	.create = create,
	.remove = remove,
	.link = link,

	.readdir = readdir,

	.readlink = readlink
};