/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 24 2023.
 */

#ifndef MLX_VOLMGR_VOLMGR_HH
#define MLX_VOLMGR_VOLMGR_HH

#include "kdk/devmgr.h"
#include "../mdf/mdfdev.hh"

struct volmgr_disk_info {
    size_t block_size;
    io_blksize_t nblocks;
};

class VolumeManager : public Device {
    public:
	VolumeManager(device_t *provider, struct volmgr_disk_info &info);
};

#endif /* MLX_VOLMGR_VOLMGR_HH */
