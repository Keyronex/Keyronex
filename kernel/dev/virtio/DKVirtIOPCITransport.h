#ifndef KRX_VIRTIO_DKVIRTIOPCITRANSPORT_H
#define KRX_VIRTIO_DKVIRTIOPCITRANSPORT_H

#include "ddk/DKVirtIOTransport.h"
#include "ddk/virtio_pcireg.h"
#include "dev/PCIBus.h"

#if defined(__aarch64__) || defined(__amd64__)
@interface DKVirtIOPCITransport
    : DKDevice <DKVirtIOTransport, DKPCIDeviceDelegate> {
    @public
	struct intr_entry m_intxEntry;
	struct pci_dev_info m_pciInfo;
	kdpc_t m_dpc;
	DKDevice<DKVirtIODelegate> *m_delegate;

	volatile struct virtio_pci_common_cfg *m_commonCfg;
	volatile void *m_deviceCfg;
	volatile uint8_t *m_isr;
	vaddr_t m_notifyBase;
	uint32_t m_notifyOffMultiplier;
}

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info;

@end
#endif

#endif /* KRX_VIRTIO_DKVIRTIOPCITRANSPORT_H */
