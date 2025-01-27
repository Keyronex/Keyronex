#ifndef KRX_DEV_DKVirtIOMMIOTransport_H
#define KRX_DEV_DKVirtIOMMIOTransport_H

#include "ddk/DKVirtIOTransport.h"

@interface DKVirtIOMMIOTransport : DKDevice <DKVirtIOTransport> {
    @public
	volatile void *m_mmio;
	int m_interrupt;
	kdpc_t m_dpc;
	DKDevice<DKVirtIODelegate> *m_delegate;
}

+ (BOOL)probeWithProvider:(DKDevice *)provider
		     mmio:(volatile void *)mmio
		interrupt:(int)interrupt;

@end

#endif /* KRX_DEV_DKVirtIOMMIOTransport_H */
