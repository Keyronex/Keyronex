/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#include <sys/errno.h>

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

	bool has_paging_fid;

	/*! 9p Qid. ninep_qid::version is the unique identifier. */
	struct ninep_qid qid;
	/*! 9p main Fid. */
	ninep_fid_t fid;
	/*! 9p Fid for pager I/O */
	ninep_fid_t paging_fid;
};

static int64_t
node_cmp(struct ninepfs_node *x, struct ninepfs_node *y)
{
	return x->qid.path - y->qid.path;
	return 0;
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

	kmem_asprintf(&objhdr.name, "9pfs%d", sequence_num++);
	attach(provider);

	negotiateVersion();
	doAttach();

	/* todo: factor away into a mount operation */
	vfs->ops = &vfsops;
	vfs->data = (uintptr_t)this;
	vfs->dev = this;

#if 0
	root_node = findOrCreateNodePair(VDIR, 0, FUSE_ROOT_ID, FUSE_ROOT_ID);
#endif
}

io_9p_request *
NinePFS::new9pRequest(struct ninep_buf *buf_in, vm_mdl_t *mdl_in,
    struct ninep_buf *buf_out, vm_mdl_t *mdl_out)
{
	io_9p_request *req = new (kmem_general) io_9p_request;
	memset(req, 0x0, sizeof(*req));

	req->ptr_in = buf_in;

	if (mdl_in) {
		req->mdl_in = mdl_in;
		req->ptr_in->data->size += PGSIZE * mdl_in->npages;
	}

	req->ptr_out = buf_out;

	if (mdl_out) {
		req->mdl_out = mdl_out;
	}

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

	switch (buf_out->data->kind) {
	case k9pAttach + 1: {
		struct ninep_qid qid;
		ninep_buf_getqid(buf_out, &qid);
		root_node = findOrCreateNodePair(VDIR, 0, &qid, 1);
		DKDevLog(this, "Attached, root FID type %d ver %d path %lu\n",
		    qid.type, qid.version, qid.path);
		break;
	}

	default: {
		kfatal("9p failure\n");
	}
	}

	return 0;
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
	if (node) {
		kassert(node->qid.path = qid->path &&
			node->qid.type == qid->type &&
			node->qid.version == qid->version);
		obj_direct_retain(node->vnode);
		ke_mutex_release(&nodecache_mutex);
		return node;
	}

	if (type == VNON) {
		ke_mutex_release(&nodecache_mutex);
		return NULL;
	}

	node = new (kmem_general) ninepfs_node;

	node->qid = *qid;
	node->fid = rdwrfid;
	node->has_paging_fid = false;
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
NinePFS::pagerFid(ninepfs_node *node, ninep_fid_t &handle_out)
{
	if (node->has_paging_fid) {
		handle_out = node->paging_fid;
		return 0;
	}

	if (node->vnode->type != VREG)
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
	ninep_buf_addu32(buf_in, O_RDWR);
	ninep_buf_close(buf_in);

	req = new9pRequest(buf_in, NULL, buf_out, NULL);
	iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	switch (buf_out->data->kind) {
	case k9pLopen + 1: {
		r = 0;
		node->has_paging_fid = true;
		node->paging_fid = new_fid;
		handle_out = new_fid;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

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

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwnames;
		ninep_buf_getu16(buf_out, &nwnames);
		kassert(nwnames == 0);
		out = newfid;
		r = 0;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	return r;
}

int
NinePFS::doGetattr(ninep_fid_t fid, vattr_t &vattr)
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

	switch (buf_out->data->kind) {
	case k9pGetattr + 1: {
		r = 0;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	uint64_t valid;

	ninep_buf_getu64(buf_out, &valid);
	kassert(valid == k9pGetattrBasic);
	ninep_buf_getqid(buf_out, NULL);
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

	ninep_buf_free(buf_in);
	ninep_buf_free(buf_out);
	iop_free(iop);

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

	switch (buf_out->data->kind) {
	case k9pWalk + 1: {
		uint16_t nwnames;

		ninep_buf_getu16(buf_out, &nwnames);
		kassert(nwnames == 1);
		ninep_buf_getqid(buf_out, &qid);

		ninep_buf_free(buf_in);
		ninep_buf_free(buf_out);

		r = self->doGetattr(newfid, vattr);
		break;
	}

	case k9pLerror + 1: {
		uint32_t err;
		ninep_buf_getu32(buf_out, &err);
		ninep_buf_free(buf_in);
		ninep_buf_free(buf_out);
		r = -err;
		break;
	}

	default: {
		kfatal("9p error\n");
	}
	}

	if (r != 0)
		return r;

	/* TODO(high) !!! clunk the fid if we got an existing node.... */

	res = self->findOrCreateNodePair(mode_to_vtype(vattr.mode), vattr.size,
	    &qid, newfid);
	if (!res)
		return -ENOENT;

	*out = res->vnode;

	return 0;
}

int
NinePFS::getattr(vnode_t *vn, vattr_t *out)
{
	NinePFS *self = (NinePFS *)vn->vfsp->data;
	ninepfs_node *node = ((ninepfs_node *)vn->data);
	return self->doGetattr(node->fid, *out);
}

int
NinePFS::read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_read(vn, buf, nbyte, off);
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

	r = pagerFid(node, myfid);
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
	.getattr = getattr,
	.lookup = lookup,
	.read = read,
};