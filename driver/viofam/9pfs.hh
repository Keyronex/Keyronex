/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#ifndef KRX_VIOFAM_9PFS_HH
#define KRX_VIOFAM_9PFS_HH

#include "9pfs_reg.h"
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

	/*! request in buffer  */
	struct ninep_buf *ptr_in;
	/* input mdl, if there is other data to be given */
	vm_mdl_t *mdl_in;

	/*! request out buffer*/
	struct ninep_buf *ptr_out;
	/* output mdl, if there is other data to be gotten */
	vm_mdl_t *mdl_out;

	/*! data for lower level */
	uint64_t lower_data;
};

RB_HEAD(ninepfs_node_rbt, ninepfs_node);

class NinePFS : public Device {
	static struct vfsops vfsops;
	static struct vnops vnops;

	/*! Counter for request tags */
	uint64_t ninep_unique = 1;
	/*! Counter for FIDs. 1 = root. (Doesn't yet handle overflow.) */
	uint32_t fid = 1;
	/*! Spinlock for counter. */
	kspinlock_t fid_lock;

	vfs_t *vfs;

	/*! Locks the node cache. */
	kmutex_t nodecache_mutex;
	/*! Indexed by 9p Qid path number. Locked by nodecache_mutex. */
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

	/*!
	 * @brief Allocate a ninepfs_node/vnode pair and cache it.
	 *
	 * @returns a ninepfs_node with a reference held, or NULL
	 */
	ninepfs_node *findOrCreateNodePair(vtype_t type, size_t size,
	    struct ninep_qid *qid, int rdwrfid);

	/*!
	 * @brief Get or create the built-in pager FID for a node.
	 *
	 * This function gets an I/O Fid or a node, or creates one if there isn't
	 * one yet. This is a special handle which is for use by the 9p ops only.
	 */
	int pagerFid(ninepfs_node *node, ninep_fid_t &handle_out);

	io_9p_request *new9pRequest(struct ninep_buf *buf_in, vm_mdl_t *mdl_in,
	    struct ninep_buf *buf_out, vm_mdl_t *mdl_out);

	int doAttach();
	int negotiateVersion();
	int doGetattr(ninep_fid_t fid, vattr_t &vattr);

	void allocFID(ninep_fid_t &out);
	int cloneNodeFID(struct ninepfs_node *node, ninep_fid_t &out);

    public:
	NinePFS(device_t *provider, vfs_t *vfs);
};

#endif /* KRX_VIOFAM_9PFS_HH */
