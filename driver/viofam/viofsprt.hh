/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 28 2023.
 */

#ifndef KRX_VIOFAM_VIOFSPRT_HH
#define KRX_VIOFAM_VIOFSPRT_HH

#include "dev/virtioreg.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"

#include "bsdqueue/list.hh"
#include "bsdqueue/slist.hh"
#include "fusefs.hh"
#include "viodev.hh"

class VirtIOFSPort : VirtIODevice {
	/*! VirtIO-FS configuration */
	struct virtio_fs_config *cfg;
	/*! Queue 0 - hiprio request virtqueue */
	virtio_queue hiprio_vq;
	/*! Queue 1 - request virtqueue */
	virtio_queue req_vq;

	/*! Fuse unique ID counter. */
	uint64_t fuse_unique = 0;

	/*! Number of requests oustanding. */
	uint64_t n_reqs_inflight = 0;

	/*! a request either on the free or inflight list. */
	struct viofs_request {
		/*! linkage in inflight or free reqs list */
		list_node<viofs_request> list_link;
		/*! generic FUSE request structure */
		io_fuse_request *fuse_req;
		/*! number of descriptors we calculated we'd need */
		uint16_t ndescs;
		/*! the first desc, by that this request is identified */
		uint16_t first_desc_id;
		bool pending;
	};

	/*! request array. */
	viofs_request *req_array;

	/*! fuse requests awaiting enough descriptors */
	slist<io_fuse_request, &io_fuse_request::slist_link> pending_reqs;
	/*! request freelist */
	list<viofs_request, &viofs_request::list_link> free_reqs;
	/*! requests in-flight */
	list<viofs_request, &viofs_request::list_link> in_flight_reqs;

	void enqueueFuseRequest(io_fuse_request *req);
	void tryStartRequests();

	void intrDpc();
	void processUsed(virtio_queue *queue, struct vring_used_elem *e);
	iop_return_t dispatchIOP(iop_t *iop);

    public:
	VirtIOFSPort(PCIDevice *provider, pci_device_info &info);
};

#endif /* KRX_VIOFAM_VIOFSPRT_HH */
