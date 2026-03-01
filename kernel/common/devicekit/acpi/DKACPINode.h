/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-26 Cloudarox Solutions.
 */
/*!
 * @file DKACPINode.h
 * @brief ACPI node interface.
 */

#ifndef ECX_ACPI_DKACPINODE_H
#define ECX_ACPI_DKACPINODE_H

#include <devicekit/DKPROMNode.h>

struct uacpi_namespace_node;

@interface DKACPINode : DKPROMNode {
	struct uacpi_namespace_node *m_nsNode;
}

@property (readonly) struct uacpi_namespace_node* nsNode;

- (instancetype)initWithNamespaceNode:(struct uacpi_namespace_node *)node;

@end;


#endif /* ECX_ACPI_DKACPINODE_H */
