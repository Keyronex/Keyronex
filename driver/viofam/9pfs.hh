/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#ifndef KRX_VIOFAM_9PFS_HH
#define KRX_VIOFAM_9PFS_HH

#include "kdk/vfs.h"

#include "../mdf/mdfdev.hh"
#include "bsdqueue/slist.hh"

/*!
 * A generic 9P request.
 */
struct io_9p_request {
	/*! list linkage */
	slist_node<io_9p_request> slist_link;

	/*! IOP with which request is associated */
	iop_t *iop;

	/*! data for lower level */
	uint64_t lower_data;
};

RB_HEAD(ninepfs_node_rbt, ninepfs_node);

class NinePFS : public Device {
	static struct vfsops vfsops;
	static struct vnops vnops;

	uint64_t ninep_unique = 1;
	vfs_t *vfs;
	kmutex_t nodecache_mutex;

	/*! Indexed by ninep I-node number. Locked by vnode_lock. */
	ninepfs_node_rbt node_rbt;

	ninepfs_node *root_node;

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

	/*! @brief IOP completion (9p port requests) */
	iop_return_t completeIOP(iop_t *iop);

#if 0
	/*!
	 * @brief Allocate a ninepfs_node/vnode pair and cache it.
	 *
	 * @returns a ninepfs_node with a reference held, or NULL
	 */
	ninepfs_node *findOrCreateNodePair(vtype_t type, size_t size,
	    ino_t ninep_ino, ino_t ninep_parent_ino);

	/*!
	 * @brief Get or create the built-in pager file handle for a node.
	 *
	 * FUSE I/O apparently needs to always go through a file handle. This
	 * function gets a file handle for a node, or creates one if there isn't
	 * one yet. This is a special handle which is for use by the FUSE ops
	 * only.
	 */
	int pagerFileHandle(ninepfs_node *node, uint64_t &handle_out);

	io_9p_request *newFuseRequest(uint32_t opcode, uint64_t nodeid,
	    uint32_t uid, uint32_t gid, uint32_t pid, void *ptr_in,
	    vm_mdl_t *mdl_in, size_t ptr_in_size, void *ptr_out,
	    vm_mdl_t *mdl_out, size_t ptr_out_size);
#endif

    public:
	NinePFS(device_t *provider, vfs_t *vfs);
};

#endif /* KRX_VIOFAM_9PFS_HH */
