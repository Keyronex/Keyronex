/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Feb 22 2023.
 */
/*!
 * @file mdfdev.h
 * @brief C++ driver framework
 */

#ifndef KRX_MDF_MDFDEV_HH
#define KRX_MDF_MDFDEV_HH

#include "kdk/devmgr.h"
#include "kdk/kmem.h"

#define kAnsiYellow "\e[0;33m"
#define kAnsiReset "\e[0m"

#define DKPrint(...) kprintf(__VA_ARGS__)
#define DKLog(SUB, ...)                                      \
	({                                                   \
		kprintf(kAnsiYellow "%s: " kAnsiReset, SUB); \
		kprintf(__VA_ARGS__);                        \
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
operator delete(void *p)
{
	kfatal("operator delete called\n");
}

inline void
operator delete(void *p, unsigned long size)
{
	kfatal("operator delete called\n");
}

extern "C" inline void
__cxa_pure_virtual()
{
	kfatal("__cxa_pure_virtual called\n");
}

template <class T>
inline constexpr void
delete_kmem(T *p)
{
	p->~T();
	kmem_free(p, sizeof(T));
}

/*inline void
operator delete(void *p, unsigned long something)
{
	kfatal("operator delete called\n");
}*/

class Device : public device_t {
	static iop_return_t dispatchIOP(device_t *dev, iop_t *iop);
	static iop_return_t completeIOP(device_t *dev, iop_t *iop);

    protected:
	void attach(device_t *provider)
	{
		dispatch = dispatchIOP;
		complete = completeIOP;
		dev_attach(this, provider);
	}
	virtual iop_return_t dispatchIOP(iop_t *iop);
	virtual iop_return_t completeIOP(iop_t *iop);
};

#endif /* KRX_MDF_MDFDEV_HH */
