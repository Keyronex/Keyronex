/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Fri Feb 24 2023.
 */

#ifndef KRX_VOLMGR_VOLMGR_HH
#define KRX_VOLMGR_VOLMGR_HH

#include "kdk/devmgr.h"

#include "../mdf/mdfdev.hh"

struct volmgr_disk_info {
	size_t block_size;
	io_blksize_t nblocks;
};

class VolumeManager : public Device {
	struct volmgr_disk_info info;

	void enumerateGPTPartitions();

    public:
	VolumeManager(device_t *provider, struct volmgr_disk_info &info);
};

#endif /* KRX_VOLMGR_VOLMGR_HH */
