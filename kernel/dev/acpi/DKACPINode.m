/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Tue Jan 28 2025.
 */
/*!
 * @file DKACPINode.h
 * @brief Defines the ACPI node device class.
 */

#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <uacpi/namespace.h>

#include "dev/acpi/DKACPINode.h"

extern DKAxis *gACPIAxis;

@implementation DKACPINode

- (instancetype)initWithNamespaceNode:(struct uacpi_namespace_node *)node
{
	if ((self = [super init])) {
		uacpi_object_name name = uacpi_namespace_node_name(node);
		m_name = kmem_alloc(5);
		memcpy(m_name, name.text, 4);
		m_name[4] = '\0';
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

@end
