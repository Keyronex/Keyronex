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

- (void)start;
- (void)enumerate;

/* Abstract methods provided by concrete subclasses. */
- (int)getPortStatus:(size_t)port status:(usb_port_status_and_change_t *)status;
- (int)setPortFeature:(size_t)port feature:(uint16_t)feature;
- (int)clearPortFeature:(size_t)port feature:(uint16_t)feature;
- (int)setupDeviceContextForPort:(size_t)port
		    deviceHandle:(out dk_usb_device_t *)handle;

@end

#if 0
@interface DKUSBExternalHub: DKUSBHub
@end
#endif

#endif /* KRX_DDK_DKUSBHUB_H */
