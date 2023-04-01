/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#include "kdk/devmgr.h"
#include "kdk/libkern.h"

#include "9pfs.hh"

struct ninep_version_hdr {
	ninep_hdr hdr;
	uint32_t msize;
	uint16_t version_len;
	char version[0];
} __attribute__((packed));

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

	io_9p_request *req;
	ninep_version_hdr *hdr_in, *hdr_out;

	hdr_in = (ninep_version_hdr *)kmem_alloc(
	    sizeof(ninep_version_hdr) + strlen("9P2000.l"));
	hdr_out = (ninep_version_hdr *)kmem_alloc(
	    sizeof(ninep_version_hdr) + 128);

	hdr_in->hdr.tag = -1;
	hdr_in->hdr.kind = k9pVersion;
	hdr_in->hdr.size = sizeof(ninep_version_hdr) + strlen("9p2000.l");
	hdr_in->version_len = 8;
	hdr_in->msize = 8288;
	memcpy(hdr_in->version, "9P2000.L", 8);

	hdr_out->hdr.size = sizeof(ninep_version_hdr) + 128;

	req = new9pRequest(&hdr_in->hdr, NULL, &hdr_out->hdr, NULL);

	iop_t *iop = iop_new_ioctl(provider, kIOCTL9PEnqueueRequest,
	    (vm_mdl_t *)req, sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	for (;;)
		;

	/* todo: factor away into a mount operation */
	vfs->ops = &vfsops;
	vfs->data = (uintptr_t)this;
	vfs->dev = this;

#if 0
	root_node = findOrCreateNodePair(VDIR, 0, FUSE_ROOT_ID, FUSE_ROOT_ID);
#endif
}

io_9p_request *
NinePFS::new9pRequest(ninep_hdr *hdr_in, vm_mdl_t *mdl_in, ninep_hdr *hdr_out,
    vm_mdl_t *mdl_out)
{
	io_9p_request *req = new (kmem_general) io_9p_request;
	memset(req, 0x0, sizeof(*req));

	req->ptr_in = hdr_in;

	if (mdl_in) {
		req->mdl_in = mdl_in;
		req->ptr_in->size += PGSIZE * mdl_in->npages;
	}

	req->ptr_out = hdr_out;

	if (mdl_out) {
		req->mdl_out = mdl_out;
	}

	return req;
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