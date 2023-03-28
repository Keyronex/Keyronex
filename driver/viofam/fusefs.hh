/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#ifndef KRX_VIOFAM_FUSEFS_HH
#define KRX_VIOFAM_FUSEFS_HH

#include "dev/fuse_kernel.h"
#include "kdk/kernel.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"

#include "../mdf/mdfdev.hh"
#include "bsdqueue/slist.hh"

/*!
 * A generic FUSE request. P
 */
struct io_fuse_request {
	/*! list linkage */
	slist_node<io_fuse_request> slist_link;

	/*! fuse in-header */
	struct fuse_in_header fuse_in_header;
	/*! fuse out-header, believe it or not */
	struct fuse_out_header fuse_out_header;

	/*! request in pointer - points to specific in-request */
	void *ptr_in;
	/*! size of what ptr_in points to */
	size_t ptr_in_size;
	/* input mdl, if there is other data to be given */
	vm_mdl_t *mdl_in;

	/*! request out pointer - points to specific out-request */
	void *ptr_out;
	/*! size of what ptr_out points to */
	size_t ptr_out_size;
	/* output mdl, if there is other data to be gotten */
	vm_mdl_t *mdl_out;

	/*! IOP with which request is associated */
	iop_t *iop;

	/*! data for lower level */
	uint64_t lower_data;
};

static_assert(sizeof(io_fuse_request) == 128,
    "io_fuse_request unexpected size");

RB_HEAD(fusefs_node_rbt, fusefs_node);

class FuseFS : public Device {
	static struct vfsops vfsops;
	static struct vnops vnops;

	uint64_t fuse_unique = 1;
	vfs_t *vfs;
	kmutex_t nodecache_mutex;

	/*! Indexed by fuse I-node number. Locked by vnode_lock. */
	fusefs_node_rbt node_rbt;

	fusefs_node *root_node;

	/*! VFS ops */
	static int root(vfs_t *vfs, vnode_t **out);
	static int vget(vfs_t *vfs, vnode_t **out, ino_t ino);

	/*! VNode ops */
	static int create(vnode_t *vn, vnode_t **out, const char *name,
	    vattr_t *attr);
	static int getattr(vnode_t *vn, vattr_t *out);
	static int getsection(vnode_t *vn, vm_object_t *out);
	static int lookup(vnode_t *vn, vnode_t **out, const char *pathname);
	static int read(vnode_t *vn, void *buf, size_t nbyte, off_t off);
	static off_t readdir(vnode_t *dvn, void *buf, size_t nbyte,
	    size_t *bytesRead, off_t seqno);
	static int readlink(vnode_t *vn, char *out);
	static int write(vnode_t *vn, void *buf, size_t nbyte, off_t off);

	/*! @brief IOP dispatch (read/write for pager). */
	iop_return_t dispatchIOP(iop_t *iop);

	/*! @brief IOP completion (Fuse FS port requests) */
	iop_return_t completeIOP(iop_t *iop);

	/*!
	 * @brief Allocate a fusefs_node/vnode pair and cache it.
	 *
	 * @returns a fusefs_node with a reference held, or NULL
	 */
	fusefs_node *findOrCreateNodePair(vtype_t type, size_t size,
	    ino_t fuse_ino, ino_t fuse_parent_ino);

	/*!
	 * @brief Get or create the built-in pager file handle for a node.
	 *
	 * FUSE I/O apparently needs to always go through a file handle. This
	 * function gets a file handle for a node, or creates one if there isn't
	 * one yet. This is a special handle which is for use by the FUSE ops
	 * only.
	 */
	int pagerFileHandle(fusefs_node *node, uint64_t &handle_out);

	io_fuse_request *newFuseRequest(uint32_t opcode, uint64_t nodeid,
	    uint32_t uid, uint32_t gid, uint32_t pid, void *ptr_in,
	    vm_mdl_t *mdl_in, size_t ptr_in_size, void *ptr_out,
	    vm_mdl_t *mdl_out, size_t ptr_out_size);

    public:
	FuseFS(device_t *provider, vfs_t *vfs);
};

#endif /* KRX_VIOFAM_FUSEFS_HH */
