/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Tue Feb 28 2023.
 */

#ifndef KRX_VIOFAM_VIOFSPRT_HH
#define KRX_VIOFAM_VIOFSPRT_HH

#include "dev/virtioreg.h"
#include "kdk/devmgr.h"
#include "kdk/kernel.h"

#include "viodev.hh"

class VirtIOFSPort : VirtIODevice {
	void intrDpc() { kfatal("not yet implemented"); }
	void processUsed(virtio_queue *queue, vring_used_elem *elem)
	{
		kfatal("not yet implemented");
	}

    public:
	VirtIOFSPort(PCIDevice *provider, pci_device_info &info);
};

#endif /* KRX_VIOFAM_VIOFSPRT_HH */
