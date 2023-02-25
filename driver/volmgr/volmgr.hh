/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Fri Feb 24 2023.
 */

#ifndef MLX_VOLMGR_VOLMGR_HH
#define MLX_VOLMGR_VOLMGR_HH

#include "../mdf/mdfdev.hh"

class VolumeManager : public Device {

    public:
	VolumeManager(device_t *provider);
};

#endif /* MLX_VOLMGR_VOLMGR_HH */
