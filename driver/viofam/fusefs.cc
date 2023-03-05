/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#include "dev/fuse_kernel.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"

#include "fusefs.hh"

struct initpair {
	// Device-readable part
	struct fuse_init_in init_in;

	// Device-writable part
	struct fuse_init_out init_out;
};

/*! Reference-counted by their vnode. Their lifespans are equivalent */
struct fusefs_node {
	/*! Entry in FuseFS::node_rbtree */
	RB_ENTRY(fusefs_node) rbt_entry;
	/*! Corresponding vnode. */
	vnode_t *vnode;
	/*! Fuse I-node number */
	ino_t inode;
};

static int64_t
node_cmp(struct fusefs_node *x, struct fusefs_node *y)
{
	return x->inode - y->inode;
}

static uint64_t sequence_num = 0;

RB_PROTOTYPE(fusefs_node_rbt, fusefs_node, rbt_entry, node_cmp);
RB_GENERATE(fusefs_node_rbt, fusefs_node, rbt_entry, node_cmp);

io_fuse_request *
FuseFS::newFuseRequest(uint32_t opcode, uint64_t nodeid, uint32_t uid,
    uint32_t gid, uint32_t pid, void *ptr_in, vm_mdl_t *mdl_in,
    size_t ptr_in_size, void *ptr_out, vm_mdl_t *mdl_out, size_t ptr_out_size)
{
	io_fuse_request *req = new (kmem_general) io_fuse_request;
	memset(req, 0x0, sizeof(*req));

	req->fuse_in_header.opcode = opcode;
	req->fuse_in_header.unique = fuse_unique++;
	req->fuse_in_header.nodeid = nodeid;
	req->fuse_in_header.uid = uid;
	req->fuse_in_header.gid = gid;
	req->fuse_in_header.pid = pid;

	if (ptr_in) {
		req->ptr_in = ptr_in;
		req->ptr_in_size = ptr_in_size;
		req->fuse_in_header.len += ptr_in_size;
	}
	if (mdl_in) {
		req->mdl_in = mdl_in;
		req->fuse_in_header.len += PGSIZE * mdl_in->npages;
	}

	if (ptr_out) {
		req->ptr_out = ptr_out;
		req->ptr_out_size = ptr_out_size;
	}
	if (mdl_out) {
		req->mdl_out = mdl_out;
	}

	return req;
}

FuseFS::FuseFS(device_t *provider, vfs_t *vfs)
{
	int r;
	vnode_t *vn;
	struct initpair pair = { 0 };

	ke_mutex_init(&nodecache_mutex);

	kmem_asprintf(&objhdr.name, "fusefs%d", sequence_num++);
	attach(provider);

	pair.init_in.major = FUSE_KERNEL_VERSION;
	pair.init_in.minor = FUSE_KERNEL_MINOR_VERSION;
	pair.init_in.flags = FUSE_MAP_ALIGNMENT;
	pair.init_in.max_readahead = PGSIZE;

	io_fuse_request *req = newFuseRequest(FUSE_INIT, FUSE_ROOT_ID, 0, 0, 0,
	    &pair.init_in, NULL, offsetof(initpair, init_out), &pair.init_out,
	    NULL, sizeof(initpair) - offsetof(initpair, init_out));

	iop_t *iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	DKDevLog(this, "Remote FS runs FUSE %d.%d\n", pair.init_out.major,
	    pair.init_out.minor);

	/* todo: factor away into a mount operation */
	vfs->ops = &vfsops;
	vfs->data = (uintptr_t)this;

	r = root(vfs, &vn);
	kassert(r == 0);
	root_node = (fusefs_node *)vn->data;

	DKDevLog(this, "Real root I-node number is %lu\n", root_node->inode);

#if 0
	fuse_open_in openin = { 0 };
	fuse_open_out openout;

	req = newFuseRequest(FUSE_OPENDIR, FUSE_ROOT_ID, 0, 0, 0, &openin, NULL,
	    sizeof(openin), &openout, NULL, sizeof(openout));

	iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest, (vm_mdl_t *)req,
	    sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	kassert(req->fuse_out_header.error == 0);

	/*! readdir */
	char *readbuf = (char *)kmem_alloc(2048);
	memset(readbuf, 0x0, 2048);
	fuse_read_in readin = { 0 };
	readin.size = 2048;
	readin.fh = openout.fh;

	req = newFuseRequest(FUSE_READDIR, FUSE_ROOT_ID, 0, 0, 0, &readin, NULL,
	    sizeof(readin), readbuf, NULL, 2048);

	iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest, (vm_mdl_t *)req,
	    sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	kdprintf("readdir done\n");

	char *dirbuf = readbuf;
	while (dirbuf < readbuf + req->fuse_out_header.len) {
		fuse_dirent *dent = (fuse_dirent *)dirbuf;
		char name[dent->namelen + 1];

		if (dent->namelen == 0)
			break;

		memcpy(name, dent->name, dent->namelen);
		name[dent->namelen] = 0;

		kdprintf("[ino %lu type %u name %s]\n", dent->ino, dent->type,
		    name);

		dirbuf += FUSE_DIRENT_SIZE(dent);
	}

	for (;;)
		;
#endif
}

int
FuseFS::root(vfs_t *vfs, vnode_t **vout)
{
	return FuseFS::vget(vfs, vout, FUSE_ROOT_ID);
}

int
FuseFS::vget(vfs_t *vfs, vnode_t **vout, ino_t ino)
{
	FuseFS *self = (FuseFS *)vfs->data;
	struct fusefs_node key, *found;
	kwaitstatus_t w;
	vnode_t *res;

	/* if root_node is still NULL, we need to do the initial load of it */
	if (ino == FUSE_ROOT_ID && self->root_node != NULL) {
		*vout = self->root_node->vnode;
		return 0;
	}

	key.inode = ino;
	w = ke_wait(&self->nodecache_mutex, "FuseFS::vget nodecache_mutex",
	    false, false, -1);
	kassert(w == kKernWaitStatusOK);

	found = RB_FIND(fusefs_node_rbt, &self->node_rbt, &key);
	if (found) {
		res = (vnode_t *)obj_direct_retain(found->vnode);
		ke_mutex_release(&self->nodecache_mutex);
		*vout = res;
		return 0;
	}

	/* not found, need to get. we need to do stupid shit because FUSE is
	 * badly designed. vget will of course fail on non-directories */
	fuse_entry_out entry_out = { 0 };
	const char *dot = ".\0";
	io_fuse_request *req;
	iop_t *iop;

	req = self->newFuseRequest(FUSE_LOOKUP, FUSE_ROOT_ID, 0, 0, 0,
	    (void *)dot, NULL, 2, &entry_out, NULL, sizeof(entry_out));
	iop = iop_new_ioctl(self->provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	kassert(req->fuse_out_header.error == 0);

	found = new (kmem_general) fusefs_node;
	found->inode = entry_out.attr.ino;
	found->vnode = new (kmem_general) vnode;
	obj_initialise_header(&found->vnode->objhdr, kObjTypeVNode);
	found->vnode->data = (uintptr_t)found;
	found->vnode->isroot = true;
	found->vnode->section = NULL;
	found->vnode->type = VDIR;
	found->vnode->vfsp = vfs;
	found->vnode->ops = NULL;
	found->vnode->vfsmountedhere = NULL;

	RB_INSERT(fusefs_node_rbt, &self->node_rbt, found);
	ke_mutex_release(&self->nodecache_mutex);

	/* this has already got one reference, being newly created */
	*vout = found->vnode;

	return 0;
}

struct vfsops FuseFS::vfsops = {
	.root = root,
	.vget = vget,
};