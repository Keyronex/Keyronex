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

#define kAnsiYellow "\e[0;33m"
#define kAnsiReset "\e[0m"

#define DKPrint(...) kprintf(__VA_ARGS__)
#define DKLog(SUB, ...)                                      \
	({                                                   \
		kdprintf(kAnsiYellow "%s: " kAnsiReset, SUB); \
		kdprintf(__VA_ARGS__);                        \
	})
#define DKDevLog(DEV, ...) DKLog((DEV->objhdr.name), __VA_ARGS__)


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
	static iop_return_t dispatchIOP(device_t *dev, iop_t *iop);

    protected:
	void attach(device_t *provider) { dispatch = dispatchIOP; dev_attach(this, provider); }
	virtual iop_return_t dispatchIOP(iop_t *iop);
};

#endif /* MLX_MDF_MDFDEV_HH */
