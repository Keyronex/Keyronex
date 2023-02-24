/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Thu Feb 23 2023.
 */

#include "kdk/devmgr.h"
#include "kdk/kernel.h"
#include "kdk/process.h"
#include "kdk/vm.h"

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
};

ksemaphore_t sem;

char *res;

VirtIODisk::VirtIODisk(PCIDevice *provider, pci_device_info &info)
    : VirtIODevice(provider, info)
{
	int r;

	kmem_asprintf(&objhdr.name, "viodisk%d", sequence_num++);

	if (!exchangeFeatures(VIRTIO_BLK_F_SEG_MAX)) {
		DKDevLog(self, "Feature exchange failed.");
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

	vmp_page_alloc(&kernel_process.vmps, true, kPageUseWired, &page);
	vaddr_t addr = (vaddr_t)P2V(page->address);
	for (int i = 0; i < ROUNDUP(io_queue.length, 3) / 3; i++) {
		vioblk_request *req = (vioblk_request *)(addr +
		    sizeof(*req) * i);
		TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);
	}

	attach(provider);

	ke_semaphore_init(&sem, 0);

	vm_mdl_t *mdl = vm_mdl_buffer_alloc(1);

	res = (char *)P2V(mdl->pages[0]->address);

	for (unsigned i = 0; i < 2; i++) {
		commonRequest(0, 1, i, mdl);

		ke_wait(&sem, "test_virtio", false, false, -1);

		res[512] = '\0';
		DKDevLog(self, "Block %d contains: \"%s\"\n", i, res);
	}
}

int
VirtIODisk::commonRequest(int kind, size_t nblocks, unsigned block,
    vm_mdl_t *mdl)
{
	uint32_t virtq_desc[7];
	ipl_t ipl;
	size_t i = 0;
	size_t ndatadescs;

#if DEBUG_DISK == 1
	DKDevLog(self, "Strategy(%d, block %lx, blocks %d)\n", strategy, block,
	    nblocks);
#endif

	ndatadescs = ROUNDUP(nblocks * 512, PGSIZE) / PGSIZE;
	kassert(ndatadescs <= 4);

	for (unsigned i = 0; i < 2 + ndatadescs; i++) {
		virtq_desc[i] = allocateDescNumOnQueue(&io_queue); // i;
	}

	ipl = ke_spinlock_acquire(&io_queue.spinlock);

	struct vioblk_request *req = TAILQ_FIRST(&free_reqs);
	TAILQ_REMOVE(&free_reqs, req, queue_entry);

	req->hdr.sector = block;
	req->hdr.type = VIRTIO_BLK_T_IN;
	req->first_desc_id = virtq_desc[0];

	io_queue.desc[virtq_desc[i]].len = sizeof(struct virtio_blk_outhdr);
	io_queue.desc[virtq_desc[i]].addr = (uint64_t)V2P(&req->hdr);
	io_queue.desc[virtq_desc[i]].flags = VRING_DESC_F_NEXT;
	io_queue.desc[virtq_desc[i]].next = virtq_desc[1];

	size_t nbytes = nblocks * 512;

	for (i = 1; i < 1 + ndatadescs; i++) {
		size_t len = MIN2(nbytes, PGSIZE);
		nbytes -= PGSIZE;
		io_queue.desc[virtq_desc[i]].len = len;
		io_queue.desc[virtq_desc[i]].addr =
		    (uint64_t)mdl->pages[i - 1]->address;
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

	ke_spinlock_release(&io_queue.spinlock, ipl);

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
		DKDevLog(self, "I/O error\n");
		for (;;)
			;
		return;
	}

	kdprintf("done request %p yielding %zu bytes\n", req, bytes);

	TAILQ_INSERT_TAIL(&free_reqs, req, queue_entry);

	ke_semaphore_release(&sem, 1);
}

void
VirtIODisk::tryStartPackets()
{
	while (true) {
		iop_t *iop;
		iop_stack_entry_t *stack;
		size_t ndescs;

		if (io_queue.nfree_descs < 3)
			return;

		iop = TAILQ_FIRST(&pending_packets);
		if (!iop)
			break;

		stack = iop_stack_current(iop);

		kassert(stack->function == kIOPTypeRead);
		kassert(stack->read.bytes <= PGSIZE ||
		    stack->read.bytes % PGSIZE == 0);
		kassert(stack->read.bytes < PGSIZE * 4);
		ndescs = 2 + stack->read.bytes;

		if (ndescs <= io_queue.nfree_descs)
			commonRequest(VIRTIO_BLK_T_IN, stack->read.bytes / 512,
			    stack->read.offset / 512, stack->mdl);
		else
			break;
	}
}