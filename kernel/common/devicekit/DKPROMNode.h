/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-26 Cloudarox Solutions.
 */
/*!
 * @file DKPROMNode.h
 * @brief PROM node abstract class.
 */

#ifndef ECX_DEVICEKIT_DKPROMNODE_H
#define ECX_DEVICEKIT_DKPROMNODE_H

#include <devicekit/DKDevice.h>
#include <devicekit/DKPCIDevice.h>

@interface DKPROMNode : DKDevice

- (instancetype)promSubNodeForBridgeAtPCIAddress:
    (DKPCIAddress)pciAddr;

@end


#endif /* ECX_DEVICEKIT_DKPROMNODE_H */
