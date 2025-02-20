/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Tue Jan 28 2025.
 */
/*!
 * @file DKACPINode.h
 * @brief Declares the ACPI node device class.
 */

#ifndef KRX_ACPI_DKACPINODE_H
#define KRX_ACPI_DKACPINODE_H

#include <ddk/DKDevice.h>
#include <ddk/DKPROM.h>

struct uacpi_namespace_node;

@interface DKACPINode : DKDevice <DKPROMNode> {
	struct uacpi_namespace_node *m_nsNode;
}

+ (void)drainStartDevicesQueue;

- (instancetype)initWithNamespaceNode:(struct uacpi_namespace_node *)node;

- (void)addToStartDevicesQueue;
- (void)startDevices;

@end

#endif /* KRX_ACPI_DKACPINODE_H */
