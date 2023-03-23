/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#include "abi-bits/errno.h"
#include "abi-bits/fcntl.h"
#include "abi-bits/stat.h"
#include "dev/fuse_kernel.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/libkern.h"
#include "kdk/object.h"
#include "kdk/objhdr.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"

#include "fusefs.hh"

#define DEBUG_FUSEFS 0

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

	/*! Do we have a pager file handle yet? */
	bool have_pager_file_handle : 1;
	/*! Pager file handle */
	uint64_t pager_file_handle;
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

static inline void
fuse_attr_to_vattr(fuse_attr &fattr, vattr_t &vattr)
{
	vattr.mode = fattr.mode;
	vattr.rdev = fattr.rdev;
	vattr.size = fattr.size;
	vattr.type = mode_to_vtype(fattr.mode);
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
	vfs->dev = this;

	root_node = findOrCreateNodePair(VDIR, 0, FUSE_ROOT_ID, FUSE_ROOT_ID);
}

fusefs_node *
FuseFS::findOrCreateNodePair(vtype_t type, size_t size, ino_t fuse_ino,
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
	obj_initialise_header(&node->vnode->vmobj.objhdr, kObjTypeVNode);
	node->vnode->data = (uintptr_t)node;
	node->vnode->type = type;
	node->vnode->vfsp = vfs;
	node->vnode->ops = &vnops;
	node->vnode->vfsmountedhere = NULL;
	node->vnode->size = size;

	node->vnode->vmobj.is_anonymous = false;
	ke_mutex_init(&node->vnode->vmobj.mutex);
	RB_INIT(&node->vnode->vmobj.page_rbtree);

	RB_INSERT(fusefs_node_rbt, &node_rbt, node);

	ke_mutex_release(&nodecache_mutex);

	return node;
}

int
FuseFS::pagerFileHandle(fusefs_node *node, uint64_t &handle_out)
{
	if (node->have_pager_file_handle) {
		handle_out = node->pager_file_handle;
		return 0;
	}

	if (node->vnode->type != VREG)
		return -EBADF;

	io_fuse_request *req;
	iop_t *iop;
	fuse_open_in open_in = { 0 };
	fuse_open_out open_out = { 0 };

	open_in.flags = O_RDWR;

	req = newFuseRequest(FUSE_OPEN, node->inode, 0, 0, 0, &open_in, NULL,
	    sizeof(open_in), &open_out, NULL, sizeof(open_out));
	iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest, (vm_mdl_t *)req,
	    sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	kassert(req->fuse_out_header.error == 0);

	node->have_pager_file_handle = true;
	node->pager_file_handle = open_out.fh;
	handle_out = open_out.fh;

	return 0;
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
	struct fusefs_node *node = self->findOrCreateNodePair(VNON, 0, ino, -1);
	if (!node)
		return -EOPNOTSUPP;
	else {
		*vout = node->vnode;
		return 0;
	}
}

int
FuseFS::getattr(vnode_t *vn, vattr_t *out)
{
	FuseFS *self = (FuseFS *)vn->vfsp->data;
	fusefs_node *node = ((fusefs_node *)vn->data);

	fuse_getattr_in getattr_in = { 0 };
	fuse_attr_out attr_out;
	io_fuse_request *req;
	iop_t *iop;

	req = self->newFuseRequest(FUSE_GETATTR, node->inode, 0, 0, 0,
	    (void *)&getattr_in, NULL, sizeof(getattr_in), &attr_out, NULL,
	    sizeof(attr_out));
	iop = iop_new_ioctl(self->provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	kassert(req->fuse_out_header.error == 0);

	fuse_attr_to_vattr(attr_out.attr, *out);

	return 0;
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
	kdprintf("I-node resulted from looking up %s: %lu\n", pathname,
	    entry_out.nodeid);
#endif

	kassert(req->fuse_out_header.error == 0);

	res = self->findOrCreateNodePair(mode_to_vtype(entry_out.attr.mode),
	    entry_out.attr.size, entry_out.nodeid, node->inode);
	if (!res)
		return -ENOENT;

	*out = res->vnode;
	return 0;
}

int
FuseFS::read(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_read(vn, buf, nbyte, off);
}

off_t
FuseFS::readdir(vnode_t *vn, void *buf, size_t nbyte, size_t *bytesRead,
    off_t seqno)
{
	FuseFS *self = (FuseFS *)vn->vfsp->data;
	fusefs_node *node = ((fusefs_node *)vn->data);

	io_fuse_request *req;
	iop_t *iop;
	fuse_read_in readin = { 0 };
	/* TODO, make read_buf smaller (by an eighth?) then nbyte
	 * since fuse dirents may be smaller than ours */
	char *read_buf = NULL;

	readin.size = nbyte;

	req = self->newFuseRequest(FUSE_READDIR, node->inode, 0, 0, 0, &readin,
	    NULL, sizeof(readin), read_buf, NULL, readin.size);
	iop = iop_new_ioctl(self->provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	kassert(req->fuse_out_header.error == 0);

	/* todo translate Fuse dirents to our dirent format */

	char *dirbuf = read_buf;
	while (dirbuf < read_buf + req->fuse_out_header.len) {
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

	*bytesRead = 0 /* number of bytes we wrote to buf goes here */;

	return req->fuse_out_header.len;
}

int
FuseFS::readlink(vnode_t *vn, char *out)
{
	FuseFS *self = (FuseFS *)vn->vfsp->data;
	fusefs_node *node = ((fusefs_node *)vn->data);

	io_fuse_request *req;
	iop_t *iop;

	req = self->newFuseRequest(FUSE_READLINK, node->inode, 0, 0, 0, NULL,
	    NULL, 0, out, NULL, 255);
	iop = iop_new_ioctl(self->provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));
	req->iop = iop;
	iop_send_sync(iop);

	kassert(req->fuse_out_header.error == 0);
	out[req->fuse_out_header.len - sizeof(fuse_out_header)] = '\0';

	return 0;
}

int
FuseFS::write(vnode_t *vn, void *buf, size_t nbyte, off_t off)
{
	return pgcache_write(vn, buf, nbyte, off);
}

iop_return_t
FuseFS::dispatchIOP(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop), *next_frame;
	io_fuse_request *req;
	fuse_read_in *read_in;
	fusefs_node *node = (fusefs_node *)frame->vnode->data;
	int r;

	/*
	 * The only IOPs for filesystems currently are pager in/out requests.
	 */

	kassert(frame->function == kIOPTypeRead);
	next_frame = iop_stack_initialise_next(iop);

	read_in = (fuse_read_in *)kmem_alloc(sizeof(fuse_read_in));
	memset(read_in, 0x0, sizeof(fuse_read_in));
	read_in->offset = frame->read.offset;
	read_in->size = frame->read.bytes;

	r = pagerFileHandle(node, read_in->fh);
	if (r != 0) {
		DKDevLog(this, "Failed to get a pager I/O handle! Error %d\n",
		    r);
		return kIOPRetCompleted;
	}

	req = newFuseRequest(FUSE_READ, node->inode, 0, 0, 0, read_in, NULL,
	    sizeof(fuse_read_in), NULL, frame->mdl, 0);
	iop_frame_setup_ioctl(next_frame, kIOCTLFuseEnqueuRequest, req,
	    sizeof(*req));
	req->iop = iop;

	return kIOPRetContinue;
}

iop_return_t
FuseFS::completeIOP(iop_t *iop)
{
	/* .... inspect the io_fuse_request in here somewhere .... */
	return kIOPRetCompleted;
}

struct vfsops FuseFS::vfsops = {
	.root = root,
	.vget = vget,
};

struct vnops FuseFS::vnops = {
	.getattr = getattr,
	.lookup = lookup,
	.read = read,
	.readlink = readlink,
};