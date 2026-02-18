/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file VirtIO9pPort.h
 * @brief VirtIO 9p port.
 */

#ifndef ECX_VIRTIO_VIRTIO9PPORT_H
#define ECX_VIRTIO_VIRTIO9PPORT_H

#include <sys/iop.h>

#include <devicekit/virtio/DKVirtIOTransport.h>

@interface VirtIO9pPort : DKDevice <DKVirtIODevice> {
	DKVirtIOTransport *m_transport;
	char *m_tag;
	iop_q_t m_iop_q;
	virtio_queue_t m_io_vq;

	size_t m_requests_n;
	struct vio9p_req *m_reqs;
	TAILQ_HEAD(, vio9p_req) m_free_reqs;
	TAILQ_HEAD(, vio9p_req) m_inflight_reqs;
}

@end

#endif /* ECX_VIRTIO_VIRTIO9PPORT_H */
