/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 03 2025.
 */

#include <ddk/DKUSBDevice.h>
#include <ddk/DKUSBHub.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>

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
			       hub:(DKUSBHub *)hub
			      port:(size_t)port;
{
	if ((self = [super init])) {
		m_controller = controller;
		m_hub = hub;
		m_port = port;

		kmem_asprintf(&m_name, "usbDevice%d", port);
	}

	return self;
}

- (void) start
{
	int r;

	r = [m_hub setupDeviceContextForPort:m_port deviceHandle:&m_devHandle];
	if (r != 0)
		kfatal("Failed to setup device context for port %zu\n", m_port);

	kprintf("%s: device on port %zu\n", [self name], m_port);
}


@end
