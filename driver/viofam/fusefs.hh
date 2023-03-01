/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#ifndef KRX_VIOFAM_FUSEFS_HH
#define KRX_VIOFAM_FUSEFS_HH

#include "dev/fuse_kernel.h"
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

class FuseFS : public Device {
	uint64_t fuse_unique = 1;

	io_fuse_request *newFuseRequest(uint32_t opcode, uint64_t nodeid,
	    uint32_t uid, uint32_t gid, uint32_t pid, void *ptr_in,
	    vm_mdl_t *mdl_in, size_t ptr_in_size, void *ptr_out,
	    vm_mdl_t *mdl_out, size_t ptr_out_size);

    public:
	FuseFS(device_t *provider);
};

#endif /* KRX_VIOFAM_FUSEFS_HH */
