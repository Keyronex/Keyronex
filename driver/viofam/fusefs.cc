/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#include "kdk/devmgr.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vm.h"

#include "fusefs.hh"

struct initpair {
	// Device-readable part
	struct fuse_in_header in;
	struct fuse_init_in init_in;

	// Device-writable part
	struct fuse_out_header out;
	struct fuse_init_out init_out;
};

struct initpair *pair;

static uint64_t sequence_num = 0;

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
	}
	if (mdl_in) {
		req->mdl_in = mdl_in;
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

FuseFS::FuseFS(device_t *provider)
{
	vm_page_t *page;

	kmem_asprintf(&objhdr.name, "fusefs%d", sequence_num++);
	attach(provider);

	/* testing shit */
	vmp_page_alloc(&kernel_process.vmps, true, kPageUseWired, &page);
	pair = (struct initpair *)P2V(page->address);

	memset(pair, 0x0, sizeof(initpair));

	pair->in.gid = 0;
	pair->in.opcode = FUSE_INIT;
	pair->in.len = offsetof(initpair, out);
	pair->in.unique = 444;
	pair->in.nodeid = FUSE_ROOT_ID;
	pair->in.pid = 0;

	pair->init_in.major = FUSE_KERNEL_VERSION;
	pair->init_in.minor = FUSE_KERNEL_MINOR_VERSION;
	pair->init_in.flags = FUSE_MAP_ALIGNMENT;
	pair->init_in.max_readahead = PGSIZE;

	io_fuse_request *req = newFuseRequest(FUSE_INIT, FUSE_ROOT_ID, 0, 0, 0,
	    &pair->in, NULL, offsetof(initpair, out), &pair->out, NULL,
	    sizeof(initpair) - offsetof(initpair, out));

	iop_t *iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	DKDevLog(this, "Remote FS runs FUSE %d.%d\n", pair->init_out.major,
	    pair->init_out.minor);

	// for (;;) ;
}