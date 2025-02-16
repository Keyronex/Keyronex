/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Sun May 12 2024.
 */

#ifndef KRX_VIRTIO_VIRTIO9PTRANSPORT_H
#define KRX_VIRTIO_VIRTIO9PTRANSPORT_H

#include <ddk/DKDevice.h>
#include <ddk/DKVirtIOTransport.h>
#include <ddk/safe_endian.h>

@interface VirtIO9pPort : DKDevice <DKVirtIODevice> {
@public
	TAILQ_TYPE_ENTRY(VirtIO9pPort) m_tagListEntry;

@protected
	DKVirtIOTransport *m_transport;

	char m_tagName[64];

	struct virtio_queue m_reqQueue;

	/*! I/O packets waiting for submission. */
	TAILQ_HEAD(, iop) pending_packets;

	/*! Base of requests array. */
	struct vio9p_req *m_requests;
	/*! Request freelist. */
	TAILQ_HEAD(, vio9p_req) free_reqs;
	/*! Virtio requests currently running. */
	TAILQ_HEAD(, vio9p_req) in_flight_reqs;
}

+ (VirtIO9pPort *)forTag:(const char *)tag;

@end

#endif /* KRX_VIRTIO_VIRTIO9PTRANSPORT_H */
