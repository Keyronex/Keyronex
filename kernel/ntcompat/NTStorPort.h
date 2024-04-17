#ifndef KRX_NTCOMPAT_STORPORT_H
#define KRX_NTCOMPAT_STORPORT_H

#include "dev/PCIBus.h"

@interface NTStorPort : DKDevice {
    @public
	struct intr_entry m_intxEntry;
	struct pci_dev_info m_info;
	struct sp_dev_ext *m_deviceExtension;
	/*! points into m_deviceExtension */
	void *m_HwDeviceExtension;

	kspinlock_t srb_deferred_completion_lock;
	struct _SCSI_REQUEST_BLOCK * srb_deferred_completion_queue;
	kdpc_t srb_deferred_completion_dpc;
}

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info;

@end

#endif /* KRX_NTCOMPAT_STORPORT_H */
