/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 03 2025.
 */

#include <ddk/DKAxis.h>
#include <ddk/DKUSBDevice.h>
#include <ddk/DKUSBHub.h>
#include <ddk/DKUSBKeyboard.h>
#include <ddk/reg/usb.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>

@implementation DKUSBController

- (int)requestDevice:(dk_usb_device_t)device
	      packet:(const void *)packet
	      length:(size_t)length
		 out:(void *)dataOut
	   outLength:(size_t)outLength
{
	kfatal("Subclass responsibility\n");
}

- (int)setupEndpointForDevice:(dk_usb_device_t)devHandle
		     endpoint:(uint8_t)endpoint
		    direction:(dk_endpoint_direction_t)dir
		maxPacketSize:(uint16_t)maxPacket
		     interval:(uint8_t)interval
	       endpointHandle:(out dk_usb_endpoint_t *)endpointHandle
{
	kfatal("Subclass responsibility\n");
}

@end

@interface DKUSBDevice ()

- (int)requestDeviceDescriptor;

@end

@implementation DKUSBDevice

@synthesize controller = m_controller;
@synthesize devHandle = m_devHandle;

- (const void *)nextDescriptorByType:(uint8_t)type after:(const void *)current;
{
	const uint8_t *ptr = current;
	const uint8_t *end = m_configDescriptor + m_configDescriptorLength;

	if (current == NULL) {
		ptr = m_configDescriptor;
} else {
	const dk_usb_descriptor_header_t *curr = current;
	ptr = (const uint8_t *)current + curr->bLength;
}

	while (ptr < end) {
		const dk_usb_descriptor_header_t *desc =
		    (const dk_usb_descriptor_header_t *)ptr;

		if (desc->bLength == 0 || ptr + desc->bLength > end)
			return NULL;

		if (desc->bDescriptorType == type)
			return desc;

		ptr += desc->bLength;
	}

	return NULL;
}

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

#if 0
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
#endif
}

- (int)requestConfigurationDescriptor
{
	dk_usb_setup_packet_t packet;
	dk_usb_configuration_descriptor_t *desc = kmem_alloc(sizeof(*desc));
	uint16_t fullLength;
	int r;

	memset(desc, 0, sizeof(*desc));

	packet.bmRequestType = 0x80; /* Device to Host, Standard, Device */
	packet.bRequest = 6;	     /* GET_DESCRIPTOR */
	packet.wValue = to_leu16(USB_DESC_TYPE_CONFIGURATION << 8); /* 1st configuration */
	packet.wIndex = to_leu16(0);
	packet.wLength = to_leu16(sizeof(*desc));

	[self requestWithPacket:&packet
			 length:sizeof(packet)
			    out:desc
		      outLength:sizeof(*desc)];

	if (desc->bLength != sizeof(dk_usb_configuration_descriptor_t) ||
	    desc->bDescriptorType != USB_DESC_TYPE_CONFIGURATION) {
		kprintf("%s: Invalid configuration descriptor received\n",
		    [self name]);
		kprintf("bLength = %d, bDescriptorType = %d\n", desc->bLength,
		    desc->bDescriptorType);
		return -1;
	}

	fullLength = from_leu16(desc->wTotalLength);

	if (fullLength < sizeof(dk_usb_configuration_descriptor_t) ||
	    fullLength > 256) {
		/* 256 = max we can heap alloc in HHDM */
		kprintf("%s: Invalid total length %d\n", [self name],
		    fullLength);
		return -1;
	}

	m_configDescriptor = kmem_alloc(fullLength);
	m_configDescriptorLength = fullLength;
	packet.wLength = to_leu16(fullLength);

	r = [self requestWithPacket:&packet
			     length:sizeof(packet)
				out:m_configDescriptor
			  outLength:fullLength];

	return r;
}

- (void)matchInterface
{
	const dk_usb_interface_descriptor_t *iface = NULL;

	while ((iface = [self nextDescriptorByType:USB_DESC_TYPE_INTERFACE
					     after:iface])) {
#if 0
		kprintf("%s: interface %zu\n", [self name], offset);
		kprintf("  bLength: %u\n", iface->bLength);
		kprintf("  bDescriptorType: %u\n", iface->bDescriptorType);
		kprintf("  bInterfaceNumber: %u\n", iface->bInterfaceNumber);
		kprintf("  bAlternateSetting: %u\n", iface->bAlternateSetting);
		kprintf("  bNumEndpoints: %u\n", iface->bNumEndpoints);
		kprintf("  bInterfaceClass: %u\n", iface->bInterfaceClass);
		kprintf("  bInterfaceSubClass: %u\n", iface->bInterfaceSubClass);
		kprintf("  bInterfaceProtocol: %u\n", iface->bInterfaceProtocol);
		kprintf("  iInterface: %u\n", iface->iInterface);
#endif

		if (iface->bInterfaceClass == USB_CLASS_HID &&
		    iface->bInterfaceSubClass == USB_SUBCLASS_BOOT &&
		    iface->bInterfaceProtocol == USB_PROTOCOL_KEYBOARD) {
			DKUSBKeyboard *kbd = [[DKUSBKeyboard alloc]
			      initWithUSBDevice:self
			    interfaceDescriptor:iface];
			[self attachChild:kbd onAxis:gDeviceAxis];
			[kbd start];
			break;
		}
	}
}

- (int)requestWithPacket:(const void *)packet
		  length:(size_t)length
		     out:(void *)dataOut
	       outLength:(size_t)outLength
{

	return [m_controller requestDevice:m_devHandle
				    packet:packet
				    length:length
				       out:dataOut
				 outLength:outLength];
}

- (void)start
{
	int r;

	r = [m_hub setupDeviceContextForPort:m_port deviceHandle:&m_devHandle];
	if (r != 0)
		kfatal("Failed to setup device context for port %zu\n", m_port);

	kprintf("%s: device on port %zu\n", [self name], m_port);

	[self requestDeviceDescriptor];
	[self requestConfigurationDescriptor];
	[self matchInterface];
}

@end
