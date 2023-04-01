/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#include "kdk/kerndefs.h"

#include "9pfs.hh"
#include "vio9pprt.hh"

static int sequence_num = 0;

struct virtio_9p_config {
	/* length of the tag name */
	uint16_t tag_len;
	/* non-NULL terminated tag name */
	uint8_t tag[];
} __attribute__((packed));

VirtIO9PPort::VirtIO9PPort(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	int r;
	char tagname[32] = { 0 };

	kmem_asprintf(&objhdr.name, "vio9pprt%d", sequence_num++);
	cfg = (virtio_9p_config *)device_cfg;

	if (!exchangeFeatures(VIRTIO_F_VERSION_1)) {
		DKDevLog(this, "Feature exchange failed.\n");
		return;
	}

	r = setupQueue(&req_vq, 0);
	if (r != 0) {
		DKDevLog(this, "failed to setup request queue: %d\n", r);
		return;
	}

	r = enableDevice();
	if (r != 0) {
		DKDevLog(this, "failed to enable device: %d\n", r);
	}

	size_t n_reqs = ROUNDUP(req_vq.length, 4) / 4;
	req_array = (vio9p_request *)kmem_alloc(sizeof(vio9p_request) * n_reqs);
	memset(req_array, 0x0, sizeof(vio9p_request) * n_reqs);

	for (size_t i = 0; i < n_reqs; i++) {
		free_reqs.insert_head(&req_array[i]);
	}

	attach(provider);

	memcpy(tagname, cfg->tag, MIN2(cfg->tag_len, 31));
	DKDevLog(this, "Tag: %s\n", tagname);

	/* todo: factor out */
	vfs_t *vfs = (vfs_t *)kmem_alloc(sizeof(vfs_t));
	vfs->vnodecovered = NULL;

	new (kmem_general) NinePFS(this, vfs);
	vfs->ops->root(vfs, &root_vnode);
}

iop_return_t
VirtIO9PPort::dispatchIOP(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);

	kassert(frame->function == kIOPTypeIOCtl);
	kassert(frame->ioctl.type == kIOCTLFuseEnqueuRequest);

	enqueue9PRequest((io_9p_request *)frame->kbuf);

	return kIOPRetPending;
}

void
VirtIO9PPort::enqueue9PRequest(io_9p_request *req)
{
	ipl_t ipl = ke_spinlock_acquire(&req_vq.spinlock);
	vio9p_request *vreq = free_reqs.remove_head();

	ke_spinlock_release(&req_vq.spinlock, ipl);

	kfatal("unimplemented\n");
}

void
VirtIO9PPort::intrDpc()
{
	ipl_t ipl = ke_spinlock_acquire(&req_vq.spinlock);
	processVirtQueue(&req_vq);
	tryStartRequests();
	ke_spinlock_release(&req_vq.spinlock, ipl);
}

void
VirtIO9PPort::processUsed(virtio_queue *queue, struct vring_used_elem *e)
{
	vio9p_request *vreq = NULL;
	struct vring_desc *desc;
	size_t descidx = e->id;

	iop_t *iop;
	size_t ndescs = 0, bytes_out = 0;

	CXXSLIST_FOREACH(vreq, &in_flight_reqs, queue_entry)
	{
		if (vreq->first_desc_id == e->id) {
			in_flight_reqs.remove(vreq);
			break;
		}
	}

	if (!vreq || vreq->first_desc_id != e->id) {
		kfatal("vio9p completion without a request\n");
		return;
	}

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

	iop = vreq->_9p_req->iop;
	free_reqs.insert_head(vreq);

	(void)bytes_out;

	iop_continue(iop, kIOPRetCompleted);
}

void
VirtIO9PPort::tryStartRequests()
{
}