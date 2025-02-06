/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Feb 05 2025.
 */

#ifndef KRX_DDK_DKUSBHUB_H
#define KRX_DDK_DKUSBHUB_H

#include <ddk/DKUSBDevice.h>
#include <ddk/reg/usb.h>

struct usb_port {
	enum port_status {
		kPortStateNotConnected,
		kPortStateResetting,
		kPortStateEnabled,
	} status;
	DKUSBDevice *dev;
};

/*!
 * @brief Abstract superclass of USB hubs, root and external.
 */
@interface DKUSBHub : DKDevice {
    @public
	TAILQ_TYPE_ENTRY(DKUSBHub) m_controllerHubsLink;

    @protected
	DKUSBController *m_controller;
	size_t m_nPorts;
	struct usb_port *m_ports;
	uint32_t m_needsEnumeration;
}

@property (readonly) dk_usb_device_t devHandle;

- (void)start;
- (void)enumerate;

/* Abstract methods provided by concrete subclasses. */
- (int)getPortStatus:(size_t)port status:(usb_port_status_and_change_t *)status;
- (int)setPortFeature:(size_t)port feature:(uint16_t)feature;
- (int)clearPortFeature:(size_t)port feature:(uint16_t)feature;
- (int)setupDeviceContextForPort:(size_t)port
		    deviceHandle:(out dk_usb_device_t *)handle;
- (void)requestReenumeration;

@end

@interface DKUSBExternalHub: DKUSBHub {
	DKUSBDevice *m_device;
	struct {
		leu16_t status;
		leu16_t changes;
	} *m_statBuffer;
	volatile uint32_t *m_statusChangeBitmap;

	const dk_usb_endpoint_descriptor_t *m_intrInEpDesc;
	dk_usb_endpoint_t m_intrInEp;
	dk_usb_transfer_t m_transfer;
}

- (instancetype)initWithController:(DKUSBController *)controller
			    device:(DKUSBDevice *)device;

@end

#endif /* KRX_DDK_DKUSBHUB_H */
