/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-26 Cloudarox Solutions.
 */
/*!
 * @file DKACPINode.m
 * @brief ACPI node implementation.
 */

#include <keyronex/intr.h>
#include <keyronex/kmem.h>

#include <libkern/lib.h>

#include <devicekit/acpi/DKACPINode.h>
#include <devicekit/DKAxis.h>

#include <uacpi/namespace.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include "keyronex/dlog.h"

#define PCI_ROOT_PNP_ID "PNP0A03"
#define PCI_EXPRESS_ROOT_PNP_ID "PNP0A08"

extern DKAxis *gACPIAxis;
kspinlock_t gStartDevicesQueueLock;
dk_device_queue_t gStartDevicesQueue =
    TAILQ_HEAD_INITIALIZER(gStartDevicesQueue);

@implementation DKACPINode

+ (void)drainStartDevicesQueue
{
	DKACPINode *device;
	ipl_t ipl = ke_spinlock_enter(&gStartDevicesQueueLock);
	while ((device = (DKACPINode *)TAILQ_FIRST(&gStartDevicesQueue)) !=
	    NULL) {
		TAILQ_REMOVE(&gStartDevicesQueue, device, m_queue_link);
		ke_spinlock_exit(&gStartDevicesQueueLock, ipl);
		[device startDevices];
		ipl = ke_spinlock_enter(&gStartDevicesQueueLock);
	}
	ke_spinlock_exit(&gStartDevicesQueueLock, ipl);
}

- (instancetype)initWithNamespaceNode:(struct uacpi_namespace_node *)node
{
	if ((self = [super init])) {
		uacpi_object_name name = uacpi_namespace_node_name(node);
		m_name = kmem_alloc(5);
		memcpy(m_name, name.text, 4);
		m_name[4] = '\0';

	//kdprintf("%s wants to start devices", self->m_name);
		for (int i = 3; i >= 0; i--) {
			if (m_name[i] == '_')
				m_name[i] = '\0';
			else
				break;
		}

		m_nsNode = node;
	}

	return self;
}

- (instancetype)promSubNodeForBridgeAtPCIAddress:(DKPCIAddress)pciAddr
{
	for (DKACPINode *subnode in [gACPIAxis childrenOf:self]) {
		uacpi_status r;
		uint64_t adr;

		r = uacpi_eval_adr(subnode->m_nsNode, &adr);
		if (r != UACPI_STATUS_OK)
			continue;

		if ((adr & 0xFFFF) == 0xFFFF) {
			adr &= ~0xFFFF;
			adr |= pciAddr.function;
		}

		if (adr == ((pciAddr.slot << 16) | pciAddr.function))
			return subnode;
	}

	return nil;
}

static uacpi_iteration_decision
iteration_callback(void *user, uacpi_namespace_node *node, uacpi_u32 depth)
{
	DKACPINode *subnode, *self = user;
	uacpi_object_type type;
	uacpi_status r;

	r = uacpi_namespace_node_type(node, &type);
	if (r != UACPI_STATUS_OK)
		return UACPI_ITERATION_DECISION_NEXT_PEER;

	if (type != UACPI_OBJECT_DEVICE &&
	    node !=
		uacpi_namespace_get_predefined(UACPI_PREDEFINED_NAMESPACE_SB))
		return UACPI_ITERATION_DECISION_NEXT_PEER;

	subnode = [[DKACPINode alloc] initWithNamespaceNode:node];
	[self attachChild:subnode onAxis:gACPIAxis];
	[subnode addToStartQueue];

	return UACPI_ITERATION_DECISION_NEXT_PEER;
}

- (void)start
{
	uacpi_namespace_for_each_child_simple(m_nsNode, iteration_callback,
	    self);
}

- (void)addToStartDevicesQueue
{
	ipl_t ipl = ke_spinlock_enter(&gStartDevicesQueueLock);
	TAILQ_INSERT_TAIL(&gStartDevicesQueue, self, m_queue_link);
	ke_spinlock_exit(&gStartDevicesQueueLock, ipl);
}

- (void)startDevices
{
	static const uacpi_char *pci_root_ids[] = { PCI_ROOT_PNP_ID,
		PCI_EXPRESS_ROOT_PNP_ID, UACPI_NULL };

	if (uacpi_device_matches_pnp_id(m_nsNode, pci_root_ids)) {
#if 0
		DKPCIRootBridge *bridge;
		uint64_t seg = 0, bus = 0;
		int r;

		r = uacpi_eval_integer(m_nsNode, "_SEG", NULL, &seg);
		if (r != UACPI_STATUS_OK && r != UACPI_STATUS_NOT_FOUND)
			kfatal("failed to evaluate _SEG: %d\n", r);

		r = uacpi_eval_integer(m_nsNode, "_BBN", NULL, &bus);
		if (r != UACPI_STATUS_OK && r != UACPI_STATUS_NOT_FOUND)
			kfatal("failed to evaluate _BBN: %d\n", r);

		bridge = [[DKPCIRootBridge alloc] initWithSegment:seg
							      bus:bus
							 promNode:self];
		[[DKACPIPlatform root] attachChild:bridge onAxis:gDeviceAxis];
		[bridge start];
#endif
	}

	for (DKACPINode *node in [gACPIAxis childrenOf:self])
		[node addToStartDevicesQueue];
}



@end
