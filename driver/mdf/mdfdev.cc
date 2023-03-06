/*
 * Copyright (c) 2023 NetaScale Object Solutions.
 * Created on Wed Feb 22 2023.
 */

#include "kdk/devmgr.h"

#include "mdfdev.hh"

int __dso_handle = 0;

extern "C" int
__cxa_atexit(void (*function)(void *), void *argument, void *dso_tag)
{
	return 0;
}

iop_return_t
Device::dispatchIOP(device_t *dev, iop_t *iop)
{
	Device *device = (Device *)dev;
	return device->dispatchIOP(iop);
}

iop_return_t
Device::completeIOP(device_t *dev, iop_t *iop)
{
	Device *device = (Device *)dev;
	return device->completeIOP(iop);
}

iop_return_t
Device::dispatchIOP(iop_t *iop)
{
	return kIOPRetContinue;
}

iop_return_t
Device::completeIOP(iop_t *iop)
{
	return kIOPRetCompleted;
}