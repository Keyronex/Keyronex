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
typedef void *dk_usb_transfer_t;

@interface DKUSBDevice : DKDevice {
	DKUSBController *m_controller;
	DKUSBHub *m_hub;
	size_t m_port;
	int m_speed;
	dk_usb_device_t m_devHandle;

	dk_usb_device_descriptor_t *m_deviceDescriptor;
	uint8_t *m_configDescriptor;
	size_t m_configDescriptorLength;
}

@property (readonly) DKUSBController *controller;
@property (readonly) dk_usb_device_t devHandle;

- (instancetype)initWithController:(DKUSBController *)controller
			       hub:(DKUSBHub *)hub
			      port:(size_t)port
			     speed:(int)speed;

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

@interface DKUSBController : DKDevice

- (void)addChildHub:(DKUSBHub *)hub;

- (void)requestReenumeration;

- (int)reconfigureDevice:(dk_usb_device_t)dev
       withMaxPacketSize:(size_t)maxPacketSize;

- (int)reconfigureDevice:(dk_usb_device_t)dev
	    asHubWithTTT:(uint32_t)ttt
		     mtt:(uint32_t)mtt;

- (int)requestDevice:(dk_usb_device_t)device
	      packet:(const void *)packet
	      length:(size_t)length
		 out:(void *)dataOut
	   outLength:(size_t)outLength;

- (int)setupDeviceContextForDeviceOnPort:(size_t)port
			 ofHubWithHandle:(dk_usb_device_t)hub
				   speed:(int)speed
			    deviceHandle:(out dk_usb_device_t *)handle;

- (int)setupEndpointForDevice:(dk_usb_device_t)devHandle
		     endpoint:(uint8_t)endpoint
			 type:(dk_endpoint_type_t)type
		    direction:(dk_endpoint_direction_t)dir
		maxPacketSize:(uint16_t)maxPacket
		     interval:(uint8_t)interval
	       endpointHandle:(out dk_usb_endpoint_t *)endpointHandle;

- (int)allocateTransfer:(out dk_usb_transfer_t *)transfer;

- (void)submitTransfer:(dk_usb_transfer_t)transfer
	      endpoint:(dk_usb_endpoint_t)endpoint
		buffer:(paddr_t)buffer
		length:(size_t)length
	      callback:(void (*)(DKUSBController *, dk_usb_transfer_t,
			   void *))callback
		 state:(void *)state;

@end

#endif /* KRX_USB_DKUSBDEVICE_H */
