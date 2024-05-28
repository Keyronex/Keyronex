#ifndef KRX_DEV_E1000_H
#define KRX_DEV_E1000_H

#include "DKNIC.h"
#include "PCIBus.h"

#if defined(__arch64__) || defined(__amd64__)

#define E1000_NDESCS 256

struct pbuf_rx;

@interface E1000 : DKNIC <DKPCIDeviceDelegate> {
	struct intr_entry m_intxEntry;
	struct pci_dev_info m_pciInfo;

	kspinlock_t m_rxLock, m_txLock, m_genLock;

	kdpc_t m_rxDpc, m_txDpc, m_linkDpc;

	vaddr_t m_reg;

	struct pbuf *m_txPendingHead, *m_txPendingTail;

	/* 256 RX descriptors. */
	struct rx_desc *m_rxDescs;
	size_t m_rxNextTail, m_rxHead;

	/* 256 TX descriptors. */
	struct tx_desc *m_txDescs;
	size_t m_txTail, m_txHead;

	/* 256 x RX buffers and 256 x TX buffers, both 2kib each. */
	vm_page_t *m_packet_buf_pages[E1000_NDESCS];
}

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info;

@end
#endif

#endif /* KRX_DEV_E1000_H */
