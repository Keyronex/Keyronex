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

typedef struct dk_usb_descriptor_header {
	uint8_t bLength;
	uint8_t bDescriptorType;
} dk_usb_descriptor_header_t;

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

/* USB Request Type fields */
#define USB_TYPE_MASK (0x03 << 5)     /* Mask for request type */
#define USB_TYPE_STANDARD (0x00 << 5) /* Standard request */
#define USB_TYPE_CLASS (0x01 << 5)    /* Class-specific request */
#define USB_TYPE_VENDOR (0x02 << 5)   /* Vendor-specific request */
#define USB_TYPE_RESERVED (0x03 << 5) /* Reserved */

/* USB Request Recipient fields */
#define USB_RECIP_MASK 0x1f	 /* Mask for recipient */
#define USB_RECIP_DEVICE 0x00	 /* Device */
#define USB_RECIP_INTERFACE 0x01 /* Interface */
#define USB_RECIP_ENDPOINT 0x02	 /* Endpoint */
#define USB_RECIP_OTHER 0x03	 /* Other */

/* HID Class-Specific Requests */
#define HID_GET_REPORT 0x01
#define HID_GET_IDLE 0x02
#define HID_GET_PROTOCOL 0x03
#define HID_SET_REPORT 0x09
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B

/* HID Protocol values */
#define HID_PROTOCOL_BOOT 0x00
#define HID_PROTOCOL_REPORT 0x01

/* Endpoint transfer types */
#define USB_ENDPOINT_XFERTYPE_MASK 0x03 /* Transfer type mask */
#define USB_ENDPOINT_XFER_CONTROL 0x00	/* Control transfer */
#define USB_ENDPOINT_XFER_ISOC 0x01	/* Isochronous transfer */
#define USB_ENDPOINT_XFER_BULK 0x02	/* Bulk transfer */
#define USB_ENDPOINT_XFER_INT 0x03	/* Interrupt transfer */

/* Endpoint direction */
#define USB_ENDPOINT_DIR_MASK 0x80 /* Direction mask */
#define USB_ENDPOINT_DIR_OUT 0x00  /* OUT = host to device */
#define USB_ENDPOINT_DIR_IN 0x80   /* IN = device to host */

/* USB HID Class codes */
#define USB_CLASS_HID 0x03
#define USB_SUBCLASS_BOOT 0x01
#define USB_PROTOCOL_KEYBOARD 0x01
#define USB_PROTOCOL_MOUSE 0x02

/* USB descriptor types */
#define USB_DESC_TYPE_CONFIGURATION 0x02
#define USB_DESC_TYPE_STRING 0x03
#define USB_DESC_TYPE_INTERFACE 0x04
#define USB_DESC_TYPE_ENDPOINT 0x05
#define USB_DESC_TYPE_HID 0x21
#define USB_DESC_TYPE_REPORT 0x22

typedef struct dk_usb_configuration_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	leu16_t wTotalLength;
	uint8_t bNumInterfaces;
	uint8_t bConfigurationValue;
	uint8_t iConfiguration;
	uint8_t bmAttributes;
	uint8_t bMaxPower;
} __attribute__((packed)) dk_usb_configuration_descriptor_t;

typedef struct dk_usb_interface_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bInterfaceNumber;
	uint8_t bAlternateSetting;
	uint8_t bNumEndpoints;
	uint8_t bInterfaceClass;
	uint8_t bInterfaceSubClass;
	uint8_t bInterfaceProtocol;
	uint8_t iInterface;
} __attribute__((packed)) dk_usb_interface_descriptor_t;

typedef struct dk_usb_endpoint_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	leu16_t wMaxPacketSize;
	uint8_t bInterval;
} __attribute__((packed)) dk_usb_endpoint_descriptor_t;

typedef struct dk_usb_hid_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	leu16_t bcdHID;
	uint8_t bCountryCode;
	uint8_t bNumDescriptors;
	uint8_t bDescriptorType0;
	leu16_t wDescriptorLength0;
} __attribute__((packed)) dk_usb_hid_descriptor_t;

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
