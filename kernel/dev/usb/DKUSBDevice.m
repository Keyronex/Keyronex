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

// #define TRACE_USB_DEVICE 2

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
			      port:(size_t)port
			     speed:(int)speed
{
	if ((self = [super init])) {
		m_controller = controller;
		m_hub = hub;
		m_port = port;
		m_speed = speed;

		kmem_asprintf(&m_name, "usb-dev-%d", port);
	}

	return self;
}

- (int)requestDeviceDescriptor
{
	dk_usb_setup_packet_t packet;
	size_t maxPacket;
	int r;

	m_deviceDescriptor = kmem_alloc(sizeof(*m_deviceDescriptor));

	packet.bmRequestType = 0x80;
	packet.bRequest = 6;
	packet.wValue = to_leu16(0x0100);
	packet.wIndex = to_leu16(0);
	packet.wLength = to_leu16(8);

	kevent_t ev;
	ke_event_init(&ev, false);
	ke_wait(&ev, "ev", 0, 0, NS_PER_S / 2);

#if TRACE_USB_DEVICE >= 2
	kprintf("%s: requesting initial device descriptor\n", [self name]);
#endif
	[self requestWithPacket:&packet
			 length:8
			    out:m_deviceDescriptor
		      outLength:8];

	if (m_speed == PORT_STATUS_OTHER_SPEED) {
		kassert(m_deviceDescriptor->bMaxPacketSize0 == 9);
		maxPacket = 512;
	} else {
		switch (m_deviceDescriptor->bMaxPacketSize0) {
		case 8:
		case 16:
		case 32:
		case 64:
			maxPacket = m_deviceDescriptor->bMaxPacketSize0;
			break;

		default:
			kfatal("Unexpected packet size %u\n",
			    m_deviceDescriptor->bMaxPacketSize0);
		}
	}
	[m_controller reconfigureDevice:m_devHandle
		      withMaxPacketSize:maxPacket];

	packet.wLength = to_leu16(sizeof(*m_deviceDescriptor));

	r = [self requestWithPacket:&packet
			 length:8
			    out:m_deviceDescriptor
		      outLength:sizeof(*m_deviceDescriptor)];

#if TRACE_USB_DEVICE
	kprintf("%s: device descriptor\n", [self name]);
	kprintf("  bLength: %u\n", m_deviceDescriptor->bLength);
	kprintf("  bDescriptorType: %u\n", m_deviceDescriptor->bDescriptorType);
	kprintf("  bcdUSB: %u\n", m_deviceDescriptor->bcdUSB);
	kprintf("  bDeviceClass: %u\n", m_deviceDescriptor->bDeviceClass);
	kprintf("  bDeviceSubClass: %u\n", m_deviceDescriptor->bDeviceSubClass);
	kprintf("  bDeviceProtocol: %u\n", m_deviceDescriptor->bDeviceProtocol);
	kprintf("  bMaxPacketSize0: %u\n", m_deviceDescriptor->bMaxPacketSize0);
	kprintf("  idVendor: 0x%04x\n",
	    from_leu16(m_deviceDescriptor->idVendor));
	kprintf("  idProduct: 0x%04x\n",
	    from_leu16(m_deviceDescriptor->idProduct));
	kprintf("  bcdDevice: %u\n", from_leu16(m_deviceDescriptor->bcdDevice));
	kprintf("  iManufacturer: %u\n", m_deviceDescriptor->iManufacturer);
	kprintf("  iProduct: %u\n", m_deviceDescriptor->iProduct);
	kprintf("  iSerialNumber: %u\n", m_deviceDescriptor->iSerialNumber);
	kprintf("  bNumConfigurations: %u\n",
	    m_deviceDescriptor->bNumConfigurations);
#endif

	return 0;
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

#if TRACE_USB_DEVICE >= 2
	kprintf("%s: requesting configuration descriptor\n", [self name]);
#endif
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

#if TRACE_USB_DEVICE >= 2
	kprintf("%s: requesting full configuration descriptor (length %d)\n",
	    [self name], fullLength);
#endif
	r = [self requestWithPacket:&packet
			     length:sizeof(packet)
				out:m_configDescriptor
			  outLength:fullLength];

#if TRACE_USB_DEVICE
	kprintf("%s: configuration descriptor\n", [self name]);
	kprintf("  bLength: %u\n", desc->bLength);
	kprintf("  bDescriptorType: %u\n", desc->bDescriptorType);
	kprintf("  wTotalLength: %u\n", from_leu16(desc->wTotalLength));
	kprintf("  bNumInterfaces: %u\n", desc->bNumInterfaces);
	kprintf("  bConfigurationValue: %u\n", desc->bConfigurationValue);
	kprintf("  iConfiguration: %u\n", desc->iConfiguration);
	kprintf("  bmAttributes: 0x%02x\n", desc->bmAttributes);
	kprintf("  bMaxPower: %u\n", desc->bMaxPower);
#endif

	return r;
}

- (void)matchInterface
{
	const dk_usb_interface_descriptor_t *iface = NULL;

	kprintf("%s: matching interface\n", [self name]);

	while ((iface = [self nextDescriptorByType:USB_DESC_TYPE_INTERFACE
					     after:iface])) {

#if TRACE_USB_DEVICE
		kprintf("%s: iface class 0x%02x:0x%02x\n", [self name],
		    iface -> bInterfaceClass, iface -> bInterfaceSubClass);
#if TRACE_USB_DEVICE >= 2
		kprintf("%s: interface\n", [self name]);
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

	r = [m_hub setupDeviceContextForPort:m_port
				       speed:m_speed
				deviceHandle:&m_devHandle];
	if (r != 0)
		kfatal("Failed to setup device context for port %zu\n", m_port);

	[self requestDeviceDescriptor];

#if TRACE_USB_DEVICE
	kprintf("%s: dev vendor 0x%04x:0x%04x class 0x%02x:0x%02x\n",
	    [self name], from_leu16(m_deviceDescriptor->idVendor),
	    from_leu16(m_deviceDescriptor->idProduct),
	    m_deviceDescriptor -> bDeviceClass,
	    m_deviceDescriptor -> bDeviceSubClass);
#endif
	[self requestConfigurationDescriptor];
;
	if (m_deviceDescriptor->bDeviceClass == 0x9) {
		DKUSBExternalHub *hub = [[DKUSBExternalHub alloc]
		    initWithController:m_controller
				  device:self];
		[self attachChild:hub onAxis:gDeviceAxis];
		[hub start];
	} else {
		[self matchInterface];
	}
}

@end
