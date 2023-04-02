/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#include "9pfs_reg.h"
#include "kdk/devmgr.h"
#include "kdk/libkern.h"

#include "9pfs.hh"

/*! Reference-counted by their vnode. Their lifespans are equivalent */
struct ninepfs_node {
	/*! Entry in NinePFS::node_rbtree. */
	RB_ENTRY(ninepfs_node) rbt_entry;
	/*! Corresponding vnode. */
	vnode_t *vnode;

	/*! 9p Qid. ninep_qid::version is the unique identifier. */
	struct ninep_qid qid;
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
	for (;;) ;
	return 0;
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
	kfatal("Unimplemented");
}

iop_return_t
NinePFS::completeIOP(iop_t *iop)
{
	kfatal("Unimplemented");
}

struct vfsops NinePFS::vfsops = {
	.root = root,
};

struct vnops NinePFS::vnops = {

};