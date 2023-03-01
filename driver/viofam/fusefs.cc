/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Mar 01 2023.
 */

#include "dev/fuse_kernel.h"
#include "kdk/devmgr.h"
#include "kdk/libkern.h"
#include "kdk/process.h"
#include "kdk/vm.h"

#include "fusefs.hh"

struct initpair {
	// Device-readable part
	struct fuse_init_in init_in;

	// Device-writable part
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

FuseFS::FuseFS(device_t *provider)
{
	vm_page_t *page;

	kmem_asprintf(&objhdr.name, "fusefs%d", sequence_num++);
	attach(provider);

	/* testing shit */
	vmp_page_alloc(&kernel_process.vmps, true, kPageUseWired, &page);
	pair = (struct initpair *)P2V(page->address);

	memset(pair, 0x0, sizeof(initpair));

	pair->init_in.major = FUSE_KERNEL_VERSION;
	pair->init_in.minor = FUSE_KERNEL_MINOR_VERSION;
	pair->init_in.flags = FUSE_MAP_ALIGNMENT;
	pair->init_in.max_readahead = PGSIZE;

	io_fuse_request *req = newFuseRequest(FUSE_INIT, FUSE_ROOT_ID, 0, 0, 0,
	    &pair->init_in, NULL, offsetof(initpair, init_out), &pair->init_out,
	    NULL, sizeof(initpair) - offsetof(initpair, init_out));

	iop_t *iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest,
	    (vm_mdl_t *)req, sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	DKDevLog(this, "Remote FS runs FUSE %d.%d\n", pair->init_out.major,
	    pair->init_out.minor);

	/*! opendir */

	fuse_open_in openin = { 0 };
	fuse_open_out openout;

	req = newFuseRequest(FUSE_OPENDIR, FUSE_ROOT_ID, 0, 0, 0, &openin, NULL,
	    sizeof(openin), &openout, NULL, sizeof(openout));

	iop = iop_new_ioctl(provider, kIOCTLFuseEnqueuRequest, (vm_mdl_t *)req,
	    sizeof(*req));

	req->iop = iop;
	iop_send_sync(iop);

	kdprintf("open done\n");

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
}