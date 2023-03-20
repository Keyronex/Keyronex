/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Thu Feb 23 2023.
 */

#include "bsdqueue/queue.h"
#include "kdk/amd64/vmamd64.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vm.h"

#include "../volmgr/volmgr.hh"
#include "viodisk.hh"

static unsigned sequence_num = 0;

struct vioblk_request {
	/* linkage in in_flight_reqs or free_reqs */
	TAILQ_ENTRY(vioblk_request) queue_entry;
	/* first descriptor */
	struct virtio_blk_outhdr hdr;
	/* final descriptor */
	uint8_t flags;
	/*! the first desc, by that this request is identified */
	uint16_t first_desc_id;
	/*! the IOP it belongs to */
	iop_t *iop;
};

ksemaphore_t sem;

char *res;

VirtIODisk::VirtIODisk(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	int r;
	volmgr_disk_info vol_info;

	kmem_asprintf(&objhdr.name, "viodisk%d", sequence_num++);

	if (!exchangeFeatures(VIRTIO_BLK_F_SEG_MAX)) {
		DKDevLog(this, "Feature exchange failed.\n");
		for (;;)
			;
		return;
	}

	cfg = (virtio_blk_config *)device_cfg;

	r = setupQueue(&io_queue, 0);
	if (r != 0) {
		DKDevLog(this, "failed to setup queue: %d\n", r);
	}

	num_queues = 1;

	r = enableDevice();
	if (r != 0) {
		DKDevLog(this, "failed to enable device: %d\n", r);
	}

	TAILQ_INIT(&free_reqs);
	TAILQ_INIT(&in_flight_reqs);
	TAILQ_INIT(&pending_packets);

	/* minimum of 3 descriptors per request; allocate reqs accordingly */
	/* TODO(high): ugly! do it better */
	vm_page_t *page;

	vmp_page_alloc(&kernel_process.map, true, kPageUseWired, &page);
	vaddr_t addr = (vaddr_t)VM_PAGE_DIRECT_MAP_ADDR(page);
	for (int i = 0; i < ROUNDUP(io_queue.length, 3) / 3; i++) {
		vioblk_request *req = (vioblk_request *)(addr +
		    sizeof(*req) * i);
		TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);
	}

	attach(provider);

#if 0
	ke_semaphore_init(&sem, 0);

	vm_mdl_t *mdl = vm_mdl_buffer_alloc(1);

	res = (char *)P2V(mdl->pages[0]->address);

	for (unsigned i = 0; i < 2; i++) {
		commonRequest(0, 1, i, mdl);

		ke_wait(&sem, "test_virtio", false, false, -1);

		res[512] = '\0';
		DKDevLog(this, "Block %d contains: \"%s\"\n", i, res);
	}
#endif

	vol_info.block_size = 512;
	vol_info.nblocks = cfg->capacity;

	new (kmem_general) VolumeManager(this, vol_info);
}

iop_return_t
VirtIODisk::dispatchIOP(iop_t *iop)
{
	iop_frame_t *frame = iop_stack_current(iop);

	kassert(frame->function == kIOPTypeRead);
	TAILQ_INSERT_TAIL(&pending_packets, iop, dev_queue_entry);

	ke_dpc_enqueue(&interrupt_dpc);

	return kIOPRetPending;
}

int
VirtIODisk::commonRequest(int kind, size_t nblocks, unsigned block,
    vm_mdl_t *mdl, iop_t *iop)
{
	uint32_t virtq_desc[7];
	size_t i = 0;
	size_t ndatadescs;

#if DEBUG_DISK == 1
	DKDevLog(this, "Strategy(%d, block %lx, blocks %d)\n", strategy, block,
	    nblocks);
#endif

	ndatadescs = ROUNDUP(nblocks * 512, PGSIZE) / PGSIZE;
	kassert(ndatadescs <= 4);

	for (unsigned i = 0; i < 2 + ndatadescs; i++) {
		virtq_desc[i] = allocateDescNumOnQueue(&io_queue); // i;
	}

	struct vioblk_request *req = TAILQ_FIRST(&free_reqs);
	TAILQ_REMOVE(&free_reqs, req, queue_entry);

#if DEBUG_VIODISK == 1
	DKDevLog(this, "op type %d (nblocks %zu offset %d; req %p)\n", kind,
	    nblocks, block, req);
#endif

	req->hdr.sector = block;
	req->hdr.type = VIRTIO_BLK_T_IN;
	req->first_desc_id = virtq_desc[0];
	req->iop = iop;

	io_queue.desc[virtq_desc[i]].len = sizeof(struct virtio_blk_outhdr);
	io_queue.desc[virtq_desc[i]].addr = (uint64_t)V2P(&req->hdr);
	io_queue.desc[virtq_desc[i]].flags = VRING_DESC_F_NEXT;
	io_queue.desc[virtq_desc[i]].next = virtq_desc[1];

	size_t nbytes = nblocks * 512;

	for (i = 1; i < 1 + ndatadescs; i++) {
		size_t len = MIN2(nbytes, PGSIZE);
		nbytes -= PGSIZE;
		io_queue.desc[virtq_desc[i]].len = len;
		io_queue.desc[virtq_desc[i]].addr = vm_mdl_paddr(mdl,
		    (i - 1) * PGSIZE);
		io_queue.desc[virtq_desc[i]].flags = VRING_DESC_F_NEXT;
		io_queue.desc[virtq_desc[i]].flags |= VRING_DESC_F_WRITE;
		io_queue.desc[virtq_desc[i]].next = virtq_desc[i + 1];
	}

	io_queue.desc[virtq_desc[i]].len = 1;
	io_queue.desc[virtq_desc[i]].addr = (uint64_t)V2P(&req->flags);
	io_queue.desc[virtq_desc[i]].flags = VRING_DESC_F_WRITE;

	TAILQ_INSERT_HEAD(&in_flight_reqs, req, queue_entry);

	submitDescNumToQueue(&io_queue, virtq_desc[0]);
	notifyQueue(&io_queue);

	return 0;
}

void
VirtIODisk::intrDpc()
{
	ipl_t ipl = ke_spinlock_acquire(&io_queue.spinlock);
	processVirtQueue(&io_queue);
	tryStartPackets();
	ke_spinlock_release(&io_queue.spinlock, ipl);
}

void
VirtIODisk::processUsed(virtio_queue *queue, struct vring_used_elem *e)
{
	struct vring_desc *dout, *dnext;
	uint16_t dnextidx;
	struct vioblk_request *req;
	size_t bytes = 0;

	TAILQ_FOREACH (req, &in_flight_reqs, queue_entry) {
		if (req->first_desc_id == e->id)
			break;
	}

	if (!req || req->first_desc_id != e->id)
		kfatal("vioblk completion without a request\n");

	TAILQ_REMOVE(&in_flight_reqs, req, queue_entry);

	dout = &QUEUE_DESC_AT(&io_queue, e->id);
	kassert(dout->flags & VRING_DESC_F_NEXT);

	dnextidx = dout->next;
	while (true) {
		dnext = &QUEUE_DESC_AT(&io_queue, dnextidx);

		if (!(dnext->flags & VRING_DESC_F_NEXT)) {
			break;
		} else {
			uint16_t old_idx = dnextidx;
			bytes += dnext->len;
			dnextidx = dnext->next;
			freeDescNumOnQueue(&io_queue, old_idx);
		}
	}

	freeDescNumOnQueue(&io_queue, e->id);
	freeDescNumOnQueue(&io_queue, dnextidx);

	if (req->flags & VIRTIO_BLK_S_IOERR ||
	    req->flags & VIRTIO_BLK_S_UNSUPP) {
		DKDevLog(this, "I/O error\n");
		for (;;)
			;
		return;
	}

#if DEBUG_VIODISK == 1
	DKDevLog(this, "done req %p yielding %zu bytes\n", req, bytes);
#endif
	/* this might be better in a separate DPC */
	iop_continue(req->iop, kIOPRetCompleted);

	TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);
}

void
VirtIODisk::tryStartPackets()
{
#if DEBUG_VIODISK == 1
	DKDevLog(this, "deferred IOP queue processing\n");
#endif
	while (true) {
		iop_t *iop;
		iop_frame_t *frame;
		size_t ndescs;

		if (io_queue.nfree_descs < 3)
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
}