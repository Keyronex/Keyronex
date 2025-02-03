/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sun Feb 02 2025.
 */
/*!
 * @file DKUSBDevice.h
 * @brief USB Device class.
 */

#ifndef KRX_USB_DKUSBDEVICE_H
#define KRX_USB_DKUSBDEVICE_H

#include <ddk/DKDevice.h>

@class DKUSBController;
@class DKUSBDevice;

/* A host controller's appropriate state. */
typedef void *dk_usb_device_t;

@protocol DKUSBDeviceDelegate

@end

@protocol DKUSBController

@end

@interface DKUSBDevice : DKDevice {
	DKUSBController *m_controller;
	dk_usb_device_t m_device;
}

- (instancetype)initWithController:(DKUSBController *)controller
			    device:(dk_usb_device_t)device;

@end

@interface DKUSBController : DKDevice {
}

- (void)requestDevice:(dk_usb_device_t)device
	       packet:(const void *)packet
	       length:(size_t)length
		  out:(void *)dataOut
	    outLength:(size_t)outLength;

@end

#endif /* KRX_USB_DKUSBDEVICE_H */
