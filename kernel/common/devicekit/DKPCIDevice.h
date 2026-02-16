/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-26 Cloudarox Solutions.
 */
/*!
 * @file DKPCIDevice.h
 * @brief PCI device.
 */

#ifndef ECX_DEVICEKIT_DKPCIDEVICE_H
#define ECX_DEVICEKIT_DKPCIDEVICE_H

#include <devicekit/DKDevice.h>

@class DKPCIBridge;
@class DKPCIDevice;
struct intr_entry;

typedef struct DKPCIAddress {
	uint16_t segment;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
} DKPCIAddress;

typedef struct DKPCIMatchData {
	uint16_t vendor;
	uint16_t device;
	uint8_t class;
	uint8_t subclass;
	uint8_t prog_if;
} DKPCIMatchData;

typedef struct DKPCIBarInfo {
	enum {
		kPCIBarMem,
		kPCIBarIO,
	} type;
	uint64_t base;
	uint64_t size;
} DKPCIBarInfo;

@protocol DKPCIDeviceMatching

+ (uint8_t)probeWithMatchData:(DKPCIMatchData *)matchData;

- (instancetype)initWithPCIDevice:(DKPCIDevice *)pciDevice;

@end


#endif /* ECX_DEVICEKIT_DKPCIDEVICE_H */
