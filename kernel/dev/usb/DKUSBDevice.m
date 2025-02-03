/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 03 2025.
 */

#include <ddk/DKUSBDevice.h>
#include <kdk/kern.h>

@implementation DKUSBController

- (void)requestDevice:(dk_usb_device_t)device
	       packet:(const void *)packet
	       length:(size_t)length
		  out:(void *)dataOut
	    outLength:(size_t)outLength
{
	kfatal("Subclass respoinsibility\n");
}

@end

@implementation DKUSBDevice

- (instancetype)initWithController:(DKUSBController *)controller
			    device:(dk_usb_device_t)device
{
	if (self = [super init]) {
		m_controller = controller;
		m_device = device;
	}

	//[m_controller ]

	return self;
}

@end
