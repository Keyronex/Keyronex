/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Sat Apr 01 2023.
 */

#ifndef KRX_VIOFAM_vio9pPRT_HH
#define KRX_VIOFAM_vio9pPRT_HH

#include "dev/virtioreg.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"

#include "9pfs.hh"
#include "bsdqueue/list.hh"
#include "bsdqueue/slist.hh"
#include "viodev.hh"

class VirtIO9PPort : VirtIODevice {
	/*! VirtIO-FS configuration */
	struct virtio_9p_config *cfg;
	/*! Queue 0 - request virtqueue */
	virtio_queue req_vq;

	/*! Number of requests oustanding. */
	uint64_t n_reqs_inflight = 0;

	/*! a request either on the free or inflight list. */
	struct vio9p_request {
		/*! linkage in inflight or free reqs list */
		list_node<vio9p_request> list_link;
		/*! generic 9p request structure */
		io_9p_request *_9p_req;
		/*! number of descriptors we calculated we'd need */
		uint16_t ndescs;
		/*! the first desc, by that this request is identified */
		uint16_t first_desc_id;
	};

	/*! request array. */
	vio9p_request *req_array;

	/*! fuse requests awaiting enough descriptors */
	slist<io_9p_request, &io_9p_request::slist_link> pending_reqs;
	/*! request freelist */
	list<vio9p_request, &vio9p_request::list_link> free_reqs;
	/*! requests in-flight */
	list<vio9p_request, &vio9p_request::list_link> in_flight_reqs;

	void enqueue9PRequest(io_9p_request *req);
	void tryStartRequests();

	void intrDpc();
	void processUsed(virtio_queue *queue, struct vring_used_elem *e);
	iop_return_t dispatchIOP(iop_t *iop);

    public:
	VirtIO9PPort(PCIDevice *provider, pci_device_info &info);
};

#endif /* KRX_VIOFAM_vio9pPRT_HH */
