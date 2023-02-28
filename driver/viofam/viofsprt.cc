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
#include "kdk/process.h"
#include "kdk/vm.h"

#include "viofsprt.hh"

typedef uint32_t le32;

struct virtio_fs_config {
	char tag[36];
	le32 num_request_queues;
	le32 notify_buf_size;
};

struct initpair {
	// Device-readable part
	struct fuse_in_header in;
	struct fuse_init_in init_in;

	// Device-writable part
	struct fuse_out_header out;
	struct fuse_init_out init_out;
};

static int sequence_num = 0;

struct initpair *pair;

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

	r = setupQueue(&hiprio_queue, 0);
	if (r != 0) {
		DKDevLog(this, "failed to setup hiprio queue: %d\n", r);
		return;
	}

	r = setupQueue(&request_queue, 1);
	if (r != 0) {
		DKDevLog(this, "failed to setup request queue: %d\n", r);
		return;
	}

	r = enableDevice();
	if (r != 0) {
		DKDevLog(this, "failed to enable device: %d\n", r);
	}

	/* testing shit */

	vm_page *page;
	uint32_t virtq_desc[2];

	vmp_page_alloc(&kernel_process.vmps, true, kPageUseWired, &page);
	pair = (struct initpair *)P2V(page->address);

	memset(pair, 0x0, sizeof(initpair));

	for (unsigned i = 0; i < 2; i++)
		virtq_desc[i] = allocateDescNumOnQueue(&request_queue);

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

	request_queue.desc[virtq_desc[0]].len = offsetof(initpair, out);
	request_queue.desc[virtq_desc[0]].addr = (uint64_t)V2P(pair);
	request_queue.desc[virtq_desc[0]].flags = VRING_DESC_F_NEXT;
	request_queue.desc[virtq_desc[0]].next = virtq_desc[1];

	request_queue.desc[virtq_desc[1]].len = sizeof(initpair) -
	    offsetof(initpair, out);
	request_queue.desc[virtq_desc[1]].addr = (uint64_t)V2P(&pair->out);
	request_queue.desc[virtq_desc[1]].flags = VRING_DESC_F_WRITE;

	submitDescNumToQueue(&request_queue, virtq_desc[0]);
	notifyQueue(&request_queue);

	for (;;)
		;
}