/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 28 2023.
 */
/*
 * Requests look like this:
 * struct virtio_fs_req {
 *		// Device-readable part
 * 		struct fuse_in_header in;
 * 		u8 datain[];
 *
 * 		// Device-writable part
 * 		struct fuse_out_header out;
 * 		u8 dataout[];
 * };
 */

#include "dev/fuse_kernel.h"
#include "dev/virtioreg.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vfs.h"
#include "kdk/vm.h"

#include "fusefs.hh"
#include "viofsprt.hh"

typedef uint32_t le32;

struct virtio_fs_config {
	char tag[36];
	le32 num_request_queues;
	le32 notify_buf_size;
};

static int sequence_num = 0;

VirtIOFSPort::VirtIOFSPort(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	int r;

	kmem_asprintf(&objhdr.name, "viofsprt%d", sequence_num++);
	cfg = (virtio_fs_config *)device_cfg;

	if (!exchangeFeatures(VIRTIO_F_VERSION_1)) {
		DKDevLog(this, "Feature exchange failed.\n");
		return;
	}

	r = setupQueue(&hiprio_vq, 0);
	if (r != 0) {
		DKDevLog(this, "failed to setup hiprio queue: %d\n", r);
		return;
	}

	r = setupQueue(&req_vq, 1);
	if (r != 0) {
		DKDevLog(this, "failed to setup request queue: %d\n", r);
		return;
	}

	r = enableDevice();
	if (r != 0) {
		DKDevLog(this, "failed to enable device: %d\n", r);
	}

	size_t n_reqs = ROUNDUP(req_vq.length, 4) / 4;
	req_array = (viofs_request *)kmem_alloc(sizeof(viofs_request) * n_reqs);
	memset(req_array, 0x0, sizeof(viofs_request) * n_reqs);

	for (size_t i = 0; i < n_reqs; i++) {
		free_reqs.insert_head(&req_array[i]);
	}

	attach(provider);

	DKDevLog(this, "Tag: %s\n", cfg->tag);

	/* todo: factor out */
	vfs_t *vfs = (vfs_t *)kmem_alloc(sizeof(vfs_t));
	vfs->vnodecovered = NULL;

	new (kmem_general) FuseFS(this, vfs);
	vfs->ops->root(vfs, &root_vnode);
}

iop_return_t
VirtIOFSPort::dispatchIOP(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);

	kassert(frame->function == kIOPTypeIOCtl);
	kassert(frame->ioctl.type == kIOCTLFuseEnqueuRequest);

	enqueueFuseRequest((io_fuse_request *)frame->kbuf);

	return kIOPRetPending;
}

void
VirtIOFSPort::enqueueFuseRequest(io_fuse_request *req)
{
	ipl_t ipl = ke_spinlock_acquire(&req_vq.spinlock);
	viofs_request *vreq = free_reqs.remove_head();

	ke_spinlock_release(&req_vq.spinlock, ipl);

	/* number of descriptors required. Two for in-header and out-header. */
	size_t ndescs = 2;
	/* descriptors allocated for req */
	uint16_t descs[14];
	/* descriptor iterator */
	size_t di = 0;

	if (req->ptr_in) {
		ndescs++;
	}
	if (req->mdl_in) {
		ndescs += req->mdl_in->npages;
	}

	if (req->ptr_out) {
		ndescs++;
	}
	if (req->mdl_out) {
		ndescs += req->mdl_out->npages;
	}

	kassert(ndescs <= 14);

	/* break this stuff off sometime soon! */
	ipl = ke_spinlock_acquire(&req_vq.spinlock);

	for (uint16_t i = 0; i < ndescs; i++) {
		descs[i] = allocateDescNumOnQueue(&req_vq);
	}

	vreq->fuse_req = req;
	vreq->ndescs = ndescs;
	vreq->first_desc_id = descs[0];

#if 0
	kdprintf("vreq %p assigned desc id %d\n", vreq, descs[0]);
#endif

	/* set the next desc, if there is a next */
#define SET_NEXT()                                                 \
	if (di + 1 < ndescs) {                                     \
		req_vq.desc[descs[di]].flags |= VRING_DESC_F_NEXT; \
		req_vq.desc[descs[di]].next = descs[di + 1];       \
	}

	/*! the fuse in-header always goes in */
	req_vq.desc[descs[di]].addr = vm_translate(
	    (vaddr_t)&req->fuse_in_header);
	req_vq.desc[descs[di]].len = sizeof(struct fuse_in_header);
	req_vq.desc[descs[di]].flags = 0;
	SET_NEXT();
	di++;

	if (req->ptr_in) {
		req_vq.desc[descs[di]].addr = vm_translate(
		    (vaddr_t)req->ptr_in);
		req_vq.desc[descs[di]].len = req->ptr_in_size;
		req_vq.desc[descs[di]].flags = 0;
		SET_NEXT();
		di++;
	}

	if (req->mdl_in) {
		/* kassert(!req->mdl->offset) */
		for (size_t i = 0; i < req->mdl_in->npages; i++) {
			req_vq.desc[descs[di]].addr = vm_mdl_paddr(req->mdl_in,
			    i * PGSIZE);
			req_vq.desc[descs[di]].len = PGSIZE;
			req_vq.desc[descs[di]].flags = 0;
			SET_NEXT();
			di++;
		}
	}

	/*! the fuse_out_header */
	req_vq.desc[descs[di]].addr = vm_translate(
	    (vaddr_t)&req->fuse_out_header);
	req_vq.desc[descs[di]].len = sizeof(struct fuse_out_header);
	req_vq.desc[descs[di]].flags = VRING_DESC_F_WRITE;
	SET_NEXT();
	di++;

	if (req->ptr_out) {
		req_vq.desc[descs[di]].addr = vm_translate(
		    (vaddr_t)req->ptr_out);
		req_vq.desc[descs[di]].len = req->ptr_out_size;
		req_vq.desc[descs[di]].flags = VRING_DESC_F_WRITE;
		SET_NEXT();
		di++;
	}

	if (req->mdl_out) {
		/* kassert(!req->mdl->offset) */
		for (size_t i = 0; i < req->mdl_out->npages; i++) {
			req_vq.desc[descs[di]].addr = vm_mdl_paddr(req->mdl_out,
			    i * PGSIZE);
			req_vq.desc[descs[di]].len = PGSIZE;
			req_vq.desc[descs[di]].flags = VRING_DESC_F_WRITE;
			SET_NEXT();
			di++;
		}
	}

	vreq->pending = true;
	in_flight_reqs.insert_head(vreq);
	n_reqs_inflight++;

	__sync_synchronize();

	submitDescNumToQueue(&req_vq, descs[0]);
	notifyQueue(&req_vq);

	ke_spinlock_release(&req_vq.spinlock, ipl);
}

void
VirtIOFSPort::intrDpc()
{
	ipl_t ipl = ke_spinlock_acquire(&req_vq.spinlock);
	processVirtQueue(&req_vq);
	tryStartRequests();
	ke_spinlock_release(&req_vq.spinlock, ipl);
}

void
VirtIOFSPort::processUsed(virtio_queue *queue, struct vring_used_elem *e)
{
	viofs_request *vreq = NULL;
	struct vring_desc *desc;
	size_t descidx = e->id;

	iop_t *iop;
	size_t ndescs = 0, bytes_out = 0;
	size_t n_reqs = ROUNDUP(req_vq.length, 4) / 4;

	for (size_t i = 0; i < n_reqs; i++) {
		if (req_array[i].pending &&
		    req_array[i].first_desc_id == e->id) {
			vreq = &req_array[i];
			n_reqs_inflight--;
			break;
		}
	}
#if 0
	CXXSLIST_FOREACH(vreq, &in_flight_reqs, queue_entry)
	{
		if (vreq->first_desc_id == e->id)
			break;
	}
#endif

	if (!vreq || vreq->first_desc_id != e->id) {
		kfatal("viofs completion without a request\n");
		return;
	}
#if 0
	else
		kdprintf("completing req %d\n", vreq->first_desc_id);
#endif

	in_flight_reqs.remove(vreq);
	vreq->pending = false;

	while (true) {
		desc = &QUEUE_DESC_AT(&req_vq, descidx);

		if (desc->flags & VRING_DESC_F_WRITE)
			bytes_out += desc->len;

		if (!(desc->flags & VRING_DESC_F_NEXT)) {
			freeDescNumOnQueue(&req_vq, descidx);
			ndescs++;
			break;
		} else {
			uint16_t oldidx = descidx;
			descidx = desc->next;
			freeDescNumOnQueue(&req_vq, oldidx);
			ndescs++;
		}
	}

	kassert(ndescs == vreq->ndescs);

	iop = vreq->fuse_req->iop;
	free_reqs.insert_head(vreq);

	(void)bytes_out;

	iop_continue(iop, kIOPRetCompleted);
}

void
VirtIOFSPort::tryStartRequests()
{
#if 0
#if DEBUG_VIODISK == 1
	DKDevLog(this, "deferred IOP queue processing\n");
	while (true) {
		iop_t *iop;
		iop_frame_t *frame;
		size_t ndescs;

		if (req_queue.nfree_descs < 3)
			return;

		iop = TAILQ_FIRST(&pending_packets);
		if (!iop)
			break;

		TAILQ_REMOVE(&pending_packets, iop, dev_queue_entry);

		frame = iop_stack_current(iop);

		kassert(frame->function == kIOPTypeRead);
		kassert(frame->read.bytes <= PGSIZE ||
		    frame->read.bytes % PGSIZE == 0);
		kassert(frame->read.bytes < PGSIZE * 4);
		ndescs = 2 + frame->read.bytes / 512;

		if (ndescs <= io_queue.nfree_descs)
			commonRequest(VIRTIO_BLK_T_IN, frame->read.bytes / 512,
			    frame->read.offset / 512, frame->mdl, iop);
		else
			break;
	}

#endif
#endif
}