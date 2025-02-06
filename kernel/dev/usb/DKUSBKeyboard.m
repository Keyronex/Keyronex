/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Thu Feb 06 2025.
 */

#include <ddk/DKUSBKeyboard.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include "ddk/DKUSBDevice.h"

@implementation DKUSBKeyboard

- (instancetype)initWithUSBDevice:(DKUSBDevice *)device
	      interfaceDescriptor:
		  (const dk_usb_interface_descriptor_t *)interfaceDescriptor
{
	if ((self = [super init])) {
		const dk_usb_endpoint_descriptor_t *ep;

		m_usbDevice = device;
		m_interfaceDescriptor = interfaceDescriptor;
		m_name = strdup("usbKeyboard");

		ep = NULL;
		while ((ep = [device nextDescriptorByType:USB_DESC_TYPE_ENDPOINT
						    after:ep])) {
			if ((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
				USB_ENDPOINT_XFER_INT &&
			    (ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) ==
				USB_ENDPOINT_DIR_IN) {
				kprintf("interrupt endpoint located\n");
				m_interruptInEndpoint = ep;
				break;
			}
		}

		if (!m_interruptInEndpoint) {
			kprintf("USB Keyboard: No interrupt IN endpoint\n");
			return nil;
		}
	}
	return self;
}

- (int)setProtocol:(uint8_t)protocol
{
	dk_usb_setup_packet_t packet;

	packet.bmRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
	packet.bRequest = HID_SET_PROTOCOL;
	packet.wValue = to_leu16(protocol);
	packet.wIndex = to_leu16(m_interfaceDescriptor->bInterfaceNumber);
	packet.wLength = to_leu16(0);

	return [m_usbDevice requestWithPacket:&packet
				       length:sizeof(packet)
					  out:NULL
				    outLength:0];
}

- (int)setIdle:(uint8_t)rate
{
	dk_usb_setup_packet_t packet;

	packet.bmRequestType = USB_TYPE_CLASS | USB_RECIP_INTERFACE;
	packet.bRequest = HID_SET_IDLE;
	packet.wValue = to_leu16((rate << 8));
	packet.wIndex = to_leu16(m_interfaceDescriptor->bInterfaceNumber);
	packet.wLength = to_leu16(0);

	return [m_usbDevice requestWithPacket:&packet
				       length:sizeof(packet)
					  out:NULL
				    outLength:0];
}

- (int)setupEndpoints
{
	uint8_t epAddr = m_interruptInEndpoint->bEndpointAddress &
	    ~USB_ENDPOINT_DIR_MASK;
	uint16_t maxPacketSize = from_leu16(m_interruptInEndpoint->wMaxPacketSize) &
	    0x7FF;

	[m_usbDevice.controller
	    setupEndpointForDevice:m_usbDevice.devHandle
			  endpoint:epAddr
			      type:kDKEndpointTypeInterrupt
			 direction:kDKEndpointDirectionIn
		     maxPacketSize:maxPacketSize
			  interval:m_interruptInEndpoint->bInterval - 1
		    endpointHandle:&m_intrInEp];

	return 0;
}

- (void)start
{
	int r;

	r = [self setProtocol:0];
	if (r != 0) {
		kprintf("USB Keyboard: Failed to set boot protocol\n");
		return;
	}

	r = [self setIdle:0];
	if (r != 0) {
		kprintf("USB Keyboard: Failed to set idle rate\n");
		return;
	}

	r = [self setupEndpoints];
	if (r != 0) {
		kprintf("USB Keyboard: Failed to setup endpoints\n");
		return;
	}
}

@end
