/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file DKAxis.h
 * @brief DeviceKit axis class interface.
 */

#ifndef ECX_DEVICEKIT_DKAXIS_H
#define ECX_DEVICEKIT_DKAXIS_H

#include <devicekit/DKDevice.h>

#include <libkern/objc/OSArray.h>
#include <libkern/objc/OSDictionary.h>

/*!
 * @brief DeviceKit axis class.
 */
@interface DKAxis : OSObject {
	const char *m_name;
	DKDevice *m_root;
	OSDictionary *m_parents;
	OSDictionary *m_children;
}

+ (void)load;

+ (instancetype)axisWithName:(const char *)name;

- (void)addChild:(DKDevice *)child ofParent:(DKDevice *)parent;

- (OSArray *)childrenOf:(DKDevice *)object;
- (OSArray *)parentsOf:(DKDevice *)object;

- (void)printSubtreeOfDevice:(DKDevice *)device;

@end

extern DKAxis *gDeviceAxis;


#endif /* ECX_DEVICEKIT_DKAXIS_H */
