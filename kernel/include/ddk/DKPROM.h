/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Sun Feb 16 2025.
 */

#ifndef KRX_DDK_DKPROM_H
#define KRX_DDK_DKPROM_H

#include <ddk/DKPCIDevice.h>

@protocol DKPROMNode

- (DKDevice<DKPROMNode> *)promSubNodeForBridgeAtPCIAddress:
    (DKPCIAddress)pciAddr;

@end

#endif /* KRX_DDK_DKPROM_H */
