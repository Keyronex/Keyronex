#ifndef KRX_DEV_DKVirtIOMMIOTransport_H
#define KRX_DEV_DKVirtIOMMIOTransport_H

#include <ddk/DKVirtIOTransport.h>

@interface DKVirtIOMMIOTransport : DKVirtIOTransport {
    @public
	volatile void *m_mmio;
	int m_interrupt;
	kdpc_t m_dpc;
	DKDevice<DKVirtIODevice> *m_delegate;

	virtio_queue_t **m_queues;
	size_t m_queues_size;
}

+ (instancetype)probeWithMMIO:(volatile void *)mmio
		interrupt:(int)interrupt;

@end

#endif /* KRX_DEV_DKVirtIOMMIOTransport_H */
