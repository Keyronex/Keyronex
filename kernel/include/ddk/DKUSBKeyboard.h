/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Thu Feb 06 2025.
 */

#ifndef KRX_USB_DKUSBKEYBOARD_H
#define KRX_USB_DKUSBKEYBOARD_H

#include <ddk/DKUSBDevice.h>

@interface DKUSBKeyboard : DKDevice {
	DKUSBDevice *m_usbDevice;
	const dk_usb_interface_descriptor_t *m_interfaceDescriptor;
	const dk_usb_endpoint_descriptor_t *m_interruptInEndpoint;
	dk_usb_endpoint_t m_intrInEp;

	dk_usb_transfer_t m_transfer;
	struct report *m_report;
}

- (instancetype)initWithUSBDevice:(DKUSBDevice *)device
	      interfaceDescriptor:
		  (const dk_usb_interface_descriptor_t *)interfaceDescriptor;

@end

#endif /* KRX_USB_DKUSBKEYBOARD_H */
