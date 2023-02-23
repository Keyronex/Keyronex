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

#define DKLog(subsys, ...) kdprintf(subsys __VA_ARGS__)
#define DKDevLog(DEV, ...) kdprintf("Device: " __VA_ARGS__)

extern struct kmem_general_t {
} kmem_general;

inline void *
operator new(size_t size, kmem_general_t)
{
	return kmem_alloc(size);
}

inline void
operator delete(void *p, unsigned long something)
{
	kfatal("operator delete called\n");
}

class Device : public device_t {
    protected:
	void attach(device_t *provider) { dev_attach(this, provider); }
};

#endif /* MLX_MDF_MDFDEV_HH */
