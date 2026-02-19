/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file VirtIOMMIOTransport.h
 * @brief MMIO transport for VirtIO.
 */

#ifndef ECX_VIRTIO_VIRTIOMMIOTRANSPORT_H
#define ECX_VIRTIO_VIRTIOMMIOTRANSPORT_H

#include <devicekit/virtio/DKVirtIOTransport.h>

@interface DKVirtIOMMIOTransport : DKVirtIOTransport {
    @public
	volatile void *m_mmio;
	ipl_t m_ipl;
	kirq_source_t m_irqSource;
	kirq_t m_irq;
	kdpc_t m_dpc;
	DKDevice<DKVirtIODevice> *m_delegate;

	virtio_queue_t **m_queues;
	size_t m_queues_size;
}

+ (instancetype)probeWithMMIO:(volatile void *)mmio
		irqSource:(kirq_source_t *)irqSource;

@end

#endif /* ECX_VIRTIO_VIRTIOMMIOTRANSPORT_H */
