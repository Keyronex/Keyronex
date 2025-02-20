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

// #define TRACE_USB_HUB 1

@implementation DKUSBHub : DKDevice

- (dk_usb_device_t)devHandle
{
	kfatal("Subclass responsibility\n");
}

- (void)start
{
	int r;

	m_ports = kmem_alloc(sizeof(struct usb_port) * m_nPorts);
	for (size_t i = 0; i < m_nPorts; i++) {
		m_ports[i].status = kPortStateNotConnected;
		m_ports[i].dev = nil;
	}

	for (size_t i = 0; i < m_nPorts; i++)
		[self setPortFeature:i feature:PORT_POWER];

	[self requestReenumeration];
}

- (void)reenumeratePort:(size_t)port
{
	usb_port_status_and_change_t status;

	[self getPortStatus:port status:&status];

#if TRACE_USB_HUB
	if (status.status || status.change || TRACE_USB_HUB >= 2)
		kprintf("%s: port %zu status=0x%04x, change=0x%04x\n",
		    [self name], port, status.status, status.change);
#endif

	if (status.change & PORT_CHANGE_CONNECT_STATUS) {
		[self clearPortFeature:port feature:C_PORT_CONNECTION];

		if (m_ports[port].status == kPortStateEnabled) {
			kprintf("warning: bad Handle this case\n");
			m_ports[port].status = kPortStateNotConnected;
		} else if (m_ports[port].status == kPortStateResetting) {
			/* do nothing */
		} else if (status.status & PORT_STATUS_CURRENT_CONNECT) {
#if TRACE_USB_HUB
			kprintf("%s: port %zu newly connected, now resetting\n",
			    [self name], port);
#endif
			[self setPortFeature:port feature:PORT_RESET];
			m_ports[port].status = kPortStateResetting;
			return;
		}
	}

	if (status.change & PORT_CHANGE_RESET) {
		[self clearPortFeature:port feature:C_PORT_RESET];
		[self clearPortFeature:port feature:C_PORT_ENABLE];

		if (m_ports[port].status != kPortStateResetting) {
			//kfatal("Handle this case\n");
			m_ports[port].status = kPortStateResetting;
		}

		if (status.status & PORT_STATUS_ENABLE_DISABLE) {
			DKUSBDevice *dev;
			int speed;

			if (status.status & PORT_STATUS_OTHER_SPEED)
				speed = PORT_STATUS_OTHER_SPEED;
			else if (status.status & PORT_STATUS_LOW_SPEED)
				speed = PORT_STATUS_LOW_SPEED;
			else if (status.status & PORT_STATUS_HIGH_SPEED)
				speed = PORT_STATUS_HIGH_SPEED;
			else
				speed = PORT_STATUS_FULL_SPEED;

#if TRACE_USB_HUB
			kprintf("%s: port %zu reset and enabled\n", [self name],
			    port);
#endif
			m_ports[port].status = kPortStateEnabled;

			dev = [[DKUSBDevice alloc]
			    initWithController:m_controller
					   hub:self
					  port:port
					 speed:speed];
			[self attachChild:dev onAxis:gDeviceAxis];

			[dev start];
		} else {
#if TRACE_USB_HUB
			kprintf("%s: port %zu reset but not enabled\n",
			    [self name], port);
#endif
			m_ports[port].status = kPortStateNotConnected;
		}
	}
}

- (void)enumerate
{
	while (__atomic_exchange_n(&m_needsEnumeration, 0, __ATOMIC_SEQ_CST)) {
#if TRACE_USB_HUB
		kprintf("%s: reenumerating\n", [self name]);
#endif
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

- (int)setupDeviceContextForPort:(size_t)port
			   speed:(int)speed
		    deviceHandle:(out dk_usb_device_t *)handle
{
	kfatal("Subclass responsibility\n");
}

- (void)requestReenumeration
{
	__atomic_store_n(&m_needsEnumeration, true, __ATOMIC_RELAXED);
	[m_controller requestReenumeration];
}

#if 0
- (int)resetupDeviceAsHub:(dk_usb_device_t)hub withNbrPorts:(uint8_t)bNbrPorts ttt:()
{

}
#endif

@end

static void hubInterruptCallback(DKUSBController *controller,
    dk_usb_transfer_t transfer, void *context);

@implementation DKUSBExternalHub

- (instancetype)initWithController:(DKUSBController *)controller
			    device:(DKUSBDevice *)device
{
	if ((self = [super init])) {
		m_controller = controller;
		m_device = device;
		m_statBuffer = kmem_alloc(sizeof(*m_statBuffer));

		kmem_asprintf(&m_name, "usbHub");

		usb_hub_descriptor_t *hubDesc = kmem_alloc(sizeof(*hubDesc));
		dk_usb_setup_packet_t setup = {
			.bmRequestType = USB_DIR_IN | USB_TYPE_CLASS |
			    USB_RECIP_DEVICE,
			.bRequest = USB_REQ_GET_DESCRIPTOR,
			.wValue = to_leu16((USB_DT_HUB << 8)),
			.wIndex = to_leu16(0),
			.wLength = to_leu16(sizeof(*hubDesc)),
		};

		[m_device requestWithPacket:&setup
				     length:sizeof(setup)
					out:hubDesc
				  outLength:sizeof(*hubDesc)];

		kassert(hubDesc->bDescriptorType == USB_DT_HUB);

		m_nPorts = hubDesc->bNbrPorts;

		[m_controller addChildHub:self];
	}

	return self;
}

- (int)setPortFeature:(size_t)port feature:(uint16_t)feature
{
	dk_usb_setup_packet_t packet;
	packet.bmRequestType = 0x23;
	packet.bRequest = USB_REQ_SET_FEATURE;
	packet.wValue = to_leu16(feature);
	packet.wIndex = to_leu16(port + 1);
	packet.wLength = to_leu16(0);

	return [m_device requestWithPacket:&packet
				    length:sizeof(packet)
				       out:NULL
				 outLength:0];
}

- (int)clearPortFeature:(size_t)port feature:(uint16_t)feature
{
	dk_usb_setup_packet_t packet;

	packet.bmRequestType = 0x23;
	packet.bRequest = USB_REQ_CLEAR_FEATURE;
	packet.wValue = to_leu16(feature);
	packet.wIndex = to_leu16(port + 1);
	packet.wLength = to_leu16(0);

	return [m_device requestWithPacket:&packet
				    length:sizeof(packet)
				       out:NULL
				 outLength:0];
}

- (int)getPortStatus:(size_t)port status:(usb_port_status_and_change_t *)status
{
	dk_usb_setup_packet_t packet;
	int r;

	packet.bmRequestType = 0xA3;
	packet.bRequest = USB_REQ_GET_STATUS;
	packet.wValue = to_leu16(0);
	packet.wIndex = to_leu16(port + 1);
	packet.wLength = to_leu16(sizeof(*status));

	r = [m_device requestWithPacket:&packet
				 length:sizeof(packet)
				    out:m_statBuffer
			      outLength:sizeof(*m_statBuffer)];

	if (r != 0)
		kfatal("getPortStatus failed\n");

	status->status = from_leu16(m_statBuffer->status);
	status->change = from_leu16(m_statBuffer->changes);

	return 0;
}

- (int)setupInterruptEndpoint
{
	const dk_usb_endpoint_descriptor_t *ep = NULL;
	uint8_t epAddr;
	uint16_t maxPacketSize;
	int r;

	while ((ep = [m_device nextDescriptorByType:USB_DESC_TYPE_ENDPOINT
					      after:ep])) {
		if (((ep->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) ==
			USB_ENDPOINT_XFER_INT) &&
		    ((ep->bEndpointAddress & USB_ENDPOINT_DIR_MASK) ==
			USB_ENDPOINT_DIR_IN)) {
			m_intrInEpDesc = (const dk_usb_endpoint_descriptor_t *)
			    ep;
			break;
		}
	}
	if (!m_intrInEpDesc) {
		kprintf("%s: No interrupt endpoint found\n", [self name]);
		return -1;
	}

	epAddr = m_intrInEpDesc->bEndpointAddress & ~USB_ENDPOINT_DIR_MASK;
	maxPacketSize = from_leu16(m_intrInEpDesc->wMaxPacketSize) & 0x7FF;

	kassert(maxPacketSize >= 1 && maxPacketSize <= 64);

	m_statusChangeBitmap = kmem_alloc(maxPacketSize);

	r = [m_controller
	    setupEndpointForDevice:m_device.devHandle
			  endpoint:epAddr
			      type:kDKEndpointTypeInterrupt
			 direction:kDKEndpointDirectionIn
		     maxPacketSize:maxPacketSize
			  interval:12 /* (m_intrInEpDesc->bInterval - 1) */
		    endpointHandle:&m_intrInEp];
	if (r != 0)
		kprintf("%s: setupEndpointForDevice failed\n", [self name]);

	return r;
}

- (void)submitHubStatusTransfer
{
	size_t length = from_leu16(m_intrInEpDesc->wMaxPacketSize) & 0x7ff;
	[m_controller submitTransfer:m_transfer
			    endpoint:m_intrInEp
			      buffer:V2P(m_statusChangeBitmap)
			      length:length
			    callback:hubInterruptCallback
			       state:self];
}

- (void)processHubStatus
{
#if TRACE_USB_HUB
	if (m_statusChangeBitmap[0] & (1 << 0)) {
		kprintf("%s: hub status change\n", [self name]);
	}

	for (size_t i = 1; i <= m_nPorts; i++) {
		/* is this bit set in m_statusChangeBitmap? */
		if (m_statusChangeBitmap[i / 8] & (1 << (i % 8))) {
			kprintf("%s: port %zu status change\n", [self name], i);
		}
	}
#endif
	[self requestReenumeration];
}

static void
hubInterruptCallback(DKUSBController *controller, dk_usb_transfer_t transfer,
    void *context)
{
	DKUSBExternalHub *hub = (DKUSBExternalHub *)context;
	[hub processHubStatus];
	[hub submitHubStatusTransfer];
}

- (void)start
{
	int r;

	r = [m_controller reconfigureDevice:m_device.devHandle
			       asHubWithTTT:0
					mtt:0];
	kassert(r == 0);

	r = [self setupInterruptEndpoint];
	kassert(r == 0);

	r = [m_device.controller allocateTransfer:&m_transfer];
	kassert(r == 0);

	[self submitHubStatusTransfer];

	[super start];
}

- (int)setupDeviceContextForPort:(size_t)port
			   speed:(int)speed
		    deviceHandle:(out dk_usb_device_t *)handle
{
	return
	    [m_controller setupDeviceContextForDeviceOnPort:port + 1
					    ofHubWithHandle:m_device.devHandle
						      speed:speed
					       deviceHandle:handle];
}

- (dk_usb_device_t)devHandle
{
	return m_device.devHandle;
}

@end
