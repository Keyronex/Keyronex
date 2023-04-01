/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#include "kdk/devmgr.h"

#include "9pfs.hh"

/*! Reference-counted by their vnode. Their lifespans are equivalent */
struct ninepfs_node {
	/*! Entry in NinePFS::node_rbtree */
	RB_ENTRY(ninepfs_node) rbt_entry;
	/*! Corresponding vnode. */
	vnode_t *vnode;
};

static int64_t
node_cmp(struct ninepfs_node *x, struct ninepfs_node *y)
{
#if 0
	return x->inode - y->inode;
#endif
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

	kfatal("Implement...\n");

	io_9p_request *req;

	iop_t *iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	DKDevLog(this, "Remote FS runs 9P\n");

	/* todo: factor away into a mount operation */
	vfs->ops = &vfsops;
	vfs->data = (uintptr_t)this;
	vfs->dev = this;

#if 0
	root_node = findOrCreateNodePair(VDIR, 0, FUSE_ROOT_ID, FUSE_ROOT_ID);
#endif
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