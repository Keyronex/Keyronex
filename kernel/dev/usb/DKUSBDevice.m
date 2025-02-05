/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 03 2025.
 */

#include <ddk/DKUSBDevice.h>
#include <ddk/DKUSBHub.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>

#include "ddk/reg/usb.h"

@implementation DKUSBController

- (void)requestDevice:(dk_usb_device_t)device
	       packet:(const void *)packet
	       length:(size_t)length
		  out:(void *)dataOut
	    outLength:(size_t)outLength
{
	kfatal("Subclass responsibility\n");
}

@end

@interface DKUSBDevice ()

- (int)requestDeviceDescriptor;

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

	[self requestDeviceDescriptor];
}

- (int)requestDeviceDescriptor
{
	dk_usb_setup_packet_t packet;
	dk_usb_device_descriptor_t *desc = kmem_alloc(sizeof(*desc));

	packet.bmRequestType = 0x80;
	packet.bRequest = 6;
	packet.wValue = to_leu16(0x0100);
	packet.wIndex = to_leu16(0);
	packet.wLength = to_leu16(sizeof(*desc));

	[self requestWithPacket:&packet
			 length:sizeof(packet)
			    out:desc
		      outLength:sizeof(*desc)];

	kprintf("%s: device descriptor\n", [self name]);
	kprintf("  bLength: %u\n", desc->bLength);
	kprintf("  bDescriptorType: %u\n", desc->bDescriptorType);
	kprintf("  bcdUSB: %u\n", desc->bcdUSB);
	kprintf("  bDeviceClass: %u\n", desc->bDeviceClass);
	kprintf("  bDeviceSubClass: %u\n", desc->bDeviceSubClass);
	kprintf("  bDeviceProtocol: %u\n", desc->bDeviceProtocol);
	kprintf("  bMaxPacketSize0: %u\n", desc->bMaxPacketSize0);
	kprintf("  idVendor: 0x%04x\n", from_leu16(desc->idVendor));
	kprintf("  idProduct: 0x%04x\n", from_leu16(desc->idProduct));
	kprintf("  bcdDevice: %u\n", from_leu16(desc->bcdDevice));
	kprintf("  iManufacturer: %u\n", desc->iManufacturer);
	kprintf("  iProduct: %u\n", desc->iProduct);
	kprintf("  iSerialNumber: %u\n", desc->iSerialNumber);
	kprintf("  bNumConfigurations: %u\n", desc->bNumConfigurations);
}

- (void)requestWithPacket:(const void *)packet
		   length:(size_t)length
		      out:(void *)dataOut
		outLength:(size_t)outLength
{

	[m_controller requestDevice:m_devHandle
			     packet:packet
			     length:length
				out:dataOut
			  outLength:outLength];
}

@end
