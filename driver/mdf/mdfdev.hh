/*
 * Copyright (c) 2023 The Melantix Project.
 * Created on Wed Feb 22 2023.
 */
/*!
 * @file mdfdev.h
 * @brief C++ driver framework
 */

#ifndef MLX_MDF_MDFDEV_HH
#define MLX_MDF_MDFDEV_HH

#include "kdk/devmgr.h"
#include "kdk/kmem.h"

extern struct kmem_general_t {
} kmem_general;

inline void *
operator new(size_t size, kmem_general_t)
{
	return kmem_alloc(size);
}

class Device : public device_t {
    protected:
	device_t *device;

	void attach(device_t *provider) { dev_attach(this, provider); }
};

#endif /* MLX_MDF_MDFDEV_HH */
