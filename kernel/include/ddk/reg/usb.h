/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Feb 03 2025.
 */


#ifndef KRX_REG_USB_H
#define KRX_REG_USB_H

#include <ddk/safe_endian.h>

typedef struct dk_usb_setup_packet {
	uint8_t bmRequestType;
	uint8_t bRequest;
	leu16_t wValue;
	leu16_t wIndex;
	leu16_t wLength;
} dk_usb_setup_packet_t;

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

/* usb 2.0 spec 11.24.2.7.1 */
typedef enum usb_wPortStatus {
	PORT_STATUS_CURRENT_CONNECT = 1 << 0,
	PORT_STATUS_ENABLE_DISABLE = 1 << 1,
	PORT_STATUS_SUSPEND = 1 << 2,
	PORT_STATUS_OVER_CURRENT = 1 << 3,
	PORT_STATUS_RESET = 1 << 4,
	PORT_STATUS_POWER = 1 << 8,
	PORT_STATUS_LOW_SPEED = 1 << 9,
	PORT_STATUS_PORT_POWER_SS = 1 << 9,
	PORT_STATUS_HIGH_SPEED = 1 << 10,
	PORT_STATUS_TEST_MODE = 1 << 11,
	PORT_STATUS_INDICATOR_CONTROL = 1 << 12,

	PORT_STATUS_FULL_SPEED = 0,	   /* fake */
	PORT_STATUS_OTHER_SPEED = 1 << 13, /* fake */
} usb_wPortStatus_t;

typedef enum usb_wPortChange {
	PORT_CHANGE_CONNECT_STATUS = 1 << 0,
	PORT_CHANGE_ENABLE_DISABLE = 1 << 1,
	PORT_CHANGE_SUSPEND = 1 << 2,
	PORT_CHANGE_OVER_CURRENT = 1 << 3,
	PORT_CHANGE_RESET = 1 << 4,
	PORT_CHANGE_RESET_BH = 1 << 5,
	PORT_CHANGE_LINK_STATE = 1 << 6,
	PORT_CHANGE_CONFIG_ERROR = 1 << 7,
} usb_wPortChange_t;

typedef struct usb_port_status_and_change {
	usb_wPortStatus_t status : 16;
	usb_wPortChange_t change : 16;
} usb_port_status_and_change_t;

/* USB 3.2 r11 spec table 10-9 */
typedef enum hub_class_feature {
	C_HUB_LOCAL_POWER = 0,
	C_HUB_OVER_CURRENT = 1,
	PORT_CONNECTION = 0,
	PORT_ENABLE = 1,
	PORT_SUSPEND = 2,
	PORT_OVER_CURRENT = 3,
	PORT_RESET = 4,
	PORT_POWER = 8,
	PORT_LOW_SPEED = 9,
	C_PORT_CONNECTION = 16,
	C_PORT_ENABLE = 17,
	C_PORT_SUSPEND = 18,
	C_PORT_OVER_CURRENT = 19,
	C_PORT_RESET = 20,
	PORT_TEST = 21,
	PORT_INDICATOR = 22,
} hub_class_feature_t;

#endif /* KRX_REG_USB_H */
