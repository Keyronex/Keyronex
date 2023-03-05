/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#include "abi-bits/errno.h"
#include "abi-bits/stat.h"
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
	/*! Fuse I-node number of parent. */
	ino_t parent_inode;
};

/*! this works as long as our defs align with Linux */
static inline vtype
mode_to_vtype(mode_t mode)
{
	switch (mode & S_IFMT) {
	case S_IFDIR:
		return VDIR;

	case S_IFCHR:
		return VCHR;

	case S_IFBLK:
		return VNON;

	case S_IFREG:
		return VREG;

	case S_IFIFO:
		return VNON;

	case S_IFLNK:
		return VLNK;

	case S_IFSOCK:
		return VNON;

	default:
		return VNON;
	}
}

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

	/*! hack for virtiofsd stupidity */
	if (root_node != NULL && nodeid == root_node->inode)
		nodeid = FUSE_ROOT_ID;

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
    : vfs(vfs)
    , root_node(NULL)
{
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

	root_node = findOrCreateNodePair(VDIR, FUSE_ROOT_ID, FUSE_ROOT_ID);

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

fusefs_node *
FuseFS::findOrCreateNodePair(vtype_t type, ino_t fuse_ino,
    ino_t fuse_parent_ino)
{
	struct fusefs_node key, *node;
	kwaitstatus_t w;

	key.inode = fuse_ino;
	w = ke_wait(&nodecache_mutex,
	    "FuseFS::findOrCreateNodePair nodecache_mutex", false, false, -1);
	kassert(w == kKernWaitStatusOK);

	node = RB_FIND(fusefs_node_rbt, &node_rbt, &key);
	if (node) {
		kassert(node->parent_inode = fuse_parent_ino);
		obj_direct_retain(node->vnode);
		ke_mutex_release(&nodecache_mutex);
		return node;
	}

	/*
	 * we can't do anything without a type and parent number because with
	 * VirtIOFS at least you can't do anything useful without the parent
	 * number; if you do lookup('..') for example you get unusable nonsense.
	 */
	if (fuse_parent_ino == (ino_t)-1 || type == VNON) {
		ke_mutex_release(&nodecache_mutex);
		return NULL;
	}

	node = new (kmem_general) fusefs_node;

	node->inode = fuse_ino;
	node->vnode = new (kmem_general) vnode;
	obj_initialise_header(&node->vnode->objhdr, kObjTypeVNode);
	node->vnode->data = (uintptr_t)node;
	node->vnode->section = NULL;
	node->vnode->type = type;
	node->vnode->vfsp = vfs;
	node->vnode->ops = &vnops;
	node->vnode->vfsmountedhere = NULL;

	RB_INSERT(fusefs_node_rbt, &node_rbt, node);

	ke_mutex_release(&nodecache_mutex);

	return node;
}

int
FuseFS::root(vfs_t *vfs, vnode_t **vout)
{
	FuseFS *self = (FuseFS *)(vfs->data);
	obj_direct_retain(self->root_node->vnode);
	*vout = self->root_node->vnode;
	return 0;
}

int
FuseFS::vget(vfs_t *vfs, vnode_t **vout, ino_t ino)
{
	FuseFS *self = (FuseFS *)vfs->data;
	struct fusefs_node *node = self->findOrCreateNodePair(VNON, ino, -1);
	if (!node)
		return -EOPNOTSUPP;
	else {
		*vout = node->vnode;
		return 0;
	}
}

int
FuseFS::lookup(vnode_t *vn, vnode_t **out, const char *pathname)
{
	FuseFS *self = (FuseFS *)vn->vfsp->data;
	fusefs_node *node = ((fusefs_node *)vn->data), *res;

	kdprintf("fusefs_lookup(vnode: %p, ino: %lu, \"%s\");\n", node->vnode,
	    node->inode, pathname);

	fuse_entry_out entry_out = { 0 };
	io_fuse_request *req;
	iop_t *iop;

	req = self->newFuseRequest(FUSE_LOOKUP, node->inode, 0, 0, 0,
	    (void *)pathname, NULL, strlen(pathname) + 1, &entry_out, NULL,
	    sizeof(entry_out));
	iop = iop_new_ioctl(self->provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

#if DEBUG_FUSEFS == 1
	kdprintf("INode resulted from looking up %s: %lu\n\n", pathname,
	    entry_out.nodeid);
#endif

	kassert(req->fuse_out_header.error == 0);

	res = self->findOrCreateNodePair(mode_to_vtype(entry_out.attr.mode),
	    entry_out.nodeid, node->inode);
	if (!res)
		return -ENOENT;

	*out = res->vnode;
	return 0;
}

struct vfsops FuseFS::vfsops = {
	.root = root,
	.vget = vget,
};

struct vnops FuseFS::vnops = {
	.lookup = lookup,
};