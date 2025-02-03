/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 03 2025.
 */


#ifndef KRX_REG_USB_H
#define KRX_REG_USB_H

#include <ddk/safe_endian.h>

typedef struct dk_usb_device_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint16_t bcdUSB;
	uint8_t bDeviceClass;
	uint8_t bDeviceSubClass;
	uint8_t bDeviceProtocol;
	uint8_t bMaxPacketSize0;
	leu16_t idVendor;
	leu16_t idProduct;
	leu16_t bcdDevice;
	uint8_t iManufacturer;
	uint8_t iProduct;
	uint8_t iSerialNumber;
	uint8_t bNumConfigurations;
} dk_usb_device_descriptor_t;

#endif /* KRX_REG_USB_H */
