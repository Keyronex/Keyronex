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
}

+ (BOOL)probeWithPCIBus:(PCIBus *)provider info:(struct pci_dev_info *)info;

@end

#endif /* KRX_NTCOMPAT_STORPORT_H */
