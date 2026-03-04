/*
 * Copyright (c) 2026 Cloudarox Solutions.
 * Created on Wed Mar 04 2026.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*!
 * @file VirtIONIC.h
 * @brief Brief explanation.
 */


#ifndef ECX_VIRTIO_VIRTIONIC_H
#define ECX_VIRTIO_VIRTIONIC_H

#include <devicekit/DKNIC.h>
#include <devicekit/virtio/DKVirtIOTransport.h>

@interface VirtIONIC : DKNIC<DKVirtIODevice> {
	DKVirtIOTransport *m_transport;

	virtio_queue_t m_rx_vq, m_tx_vq;

	size_t m_rx_bufs_n;
	struct vionic_rx_req *m_rx_reqs;
	vm_page_t **m_rx_req_pages;
	TAILQ_HEAD(, vionic_rx_req) m_rx_free_reqs;
	TAILQ_HEAD(, vionic_rx_req) m_rx_inflight_reqs;

	size_t m_tx_reqs_n;
	struct vionic_tx_req *m_tx_reqs;
	TAILQ_HEAD(, vionic_tx_req) m_tx_free_reqs;
	TAILQ_HEAD(, vionic_tx_req) m_tx_inflight_reqs;
}


/* implements for DKNIC */
- (int)transmitPacket:(mblk_t *)mp;

@end

#endif /* ECX_VIRTIO_VIRTIONIC_H */
