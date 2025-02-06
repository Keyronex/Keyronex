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

#include "ddk/reg/usb.h"

@class DKUSBController;
@class DKUSBHub;
@class DKUSBDevice;

typedef enum dk_endpoint_direction {
	kDKEndpointDirectionOut = 0,
	kDKEndpointDirectionIn = 1,
	kDKEndpointDirectionBidi = 2,
} dk_endpoint_direction_t;

typedef enum dk_endpoint_type {
	kDKEndpointTypeControl = 0,
	kDKEndpointTypeIsochronous = 1,
	kDKEndpointTypeBulk = 2,
	kDKEndpointTypeInterrupt = 3,
} dk_endpoint_type_t;

/* A host controller's appropriate state. */
typedef void *dk_usb_device_t;
typedef void *dk_usb_endpoint_t;

@protocol DKUSBDeviceDelegate

@end

@protocol DKUSBController

@end

@interface DKUSBDevice : DKDevice {
	DKUSBController *m_controller;
	DKUSBHub *m_hub;
	size_t m_port;
	dk_usb_device_t m_devHandle;

	uint8_t *m_configDescriptor;
	size_t m_configDescriptorLength;
}

@property (readonly) DKUSBController *controller;
@property (readonly) dk_usb_device_t devHandle;

- (instancetype)initWithController:(DKUSBController *)controller
			       hub:(DKUSBHub *)hub
			      port:(size_t)port;

/*!
 * @brief Make a control request to the device.
 */
- (int)requestWithPacket:(const void *)packet
		  length:(size_t)length
		     out:(void *)dataOut
	       outLength:(size_t)outLength;

/*!
 * @brief Get next configuration descriptor of some type, optionally after one.
 */
- (const void *)nextDescriptorByType:(uint8_t)type after:(const void *)current;

@end

@interface DKUSBController : DKDevice {
}

- (int)requestDevice:(dk_usb_device_t)device
	      packet:(const void *)packet
	      length:(size_t)length
		 out:(void *)dataOut
	   outLength:(size_t)outLength;

- (int)setupEndpointForDevice:(dk_usb_device_t)devHandle
		     endpoint:(uint8_t)endpoint
			 type:(dk_endpoint_type_t)type
		    direction:(dk_endpoint_direction_t)dir
		maxPacketSize:(uint16_t)maxPacket
		     interval:(uint8_t)interval
	       endpointHandle:(out dk_usb_endpoint_t *)endpointHandle;

@end

#endif /* KRX_USB_DKUSBDEVICE_H */
