/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Feb 05 2025.
 */

#include <ddk/DKAxis.h>
#include <ddk/DKUSBDevice.h>
#include <ddk/DKUSBHub.h>
#include <ddk/reg/usb.h>
#include <kdk/kern.h>
#include <kdk/kmem.h>

@implementation DKUSBHub : DKDevice

- (void)start
{
	m_ports = kmem_alloc(sizeof(struct usb_port) * m_nPorts);
	for (size_t i = 0; i < m_nPorts; i++) {
		m_ports[i].status = kPortStateNotConnected;
		m_ports[i].dev = nil;
	}

	for (size_t i = 0; i < m_nPorts; i++)
		[self setPortFeature:i feature:PORT_STATUS_POWER];

	[self requestReenumeration];
}

- (void)reenumeratePort:(size_t)port
{
	usb_port_status_and_change_t status;

	[self getPortStatus:port status:&status];

	if (status.change & PORT_CHANGE_CONNECT_STATUS) {
		[self clearPortFeature:port feature:C_PORT_CONNECTION];

		if (m_ports[port].status == kPortStateEnabled) {
			kfatal("Handle this case\n");
		} else if (m_ports[port].status == kPortStateResetting) {
			/* do nothing */
		} else if (status.status & PORT_STATUS_CURRENT_CONNECT) {
			kprintf("%s: port %zu newly connected, now resetting\n",
			    [self name], port);
			[self setPortFeature:port feature:PORT_RESET];
			m_ports[port].status = kPortStateResetting;
			return;
		}
	}

	if (status.change & PORT_CHANGE_RESET) {
		[self clearPortFeature:port feature:C_PORT_RESET];

		if (m_ports[port].status != kPortStateResetting) {
			kfatal("Handle this case\n");
		}

		if (status.status & PORT_STATUS_ENABLE_DISABLE) {
			DKUSBDevice *dev;

			kprintf("%s: port %zu reset and enabled\n", [self name],
			    port);
			m_ports[port].status = kPortStateEnabled;

			dev = [[DKUSBDevice alloc]
			    initWithController:m_controller
					   hub:self
					  port:port];
			[self attachChild:dev onAxis:gDeviceAxis];

			[dev start];
		} else {
			kprintf("%s: port %zu reset but not enabled\n",
			    [self name], port);
			m_ports[port].status = kPortStateNotConnected;
		}
	}
}

- (void)enumerate
{
	kprintf("%s: enumerating\n", [self name]);
	while (__atomic_exchange_n(&m_needsEnumeration, 0, __ATOMIC_SEQ_CST)) {
		for (size_t i = 0; i < m_nPorts; i++) {
			[self reenumeratePort:i];
		}
	}
}

- (int)getPortStatus:(size_t)port status:(usb_port_status_and_change_t *)status
{
	kfatal("Subclass responsibility\n");
}

- (int)setPortFeature:(size_t)port feature:(uint16_t)feature
{
	kfatal("Subclass responsibility\n");
}

- (int)clearPortFeature:(size_t)port feature:(uint16_t)feature
{
	kfatal("Subclass responsibility\n");
}

- (void)requestReenumeration
{
	kfatal("Subclass responsibility\n");
}

@end
