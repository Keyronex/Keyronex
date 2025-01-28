/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */
/*!
 * @file DKAxis.h
 * @brief DeviceKit axis class and related definitions.
 */

#ifndef KRX_DEV_DKAXIS_H
#define KRX_DEV_DKAXIS_H

#include <ddk/DKDevice.h>
#include <libkern/OSDictionary.h>

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

- (void)printSubtreeOfDevice:(DKDevice *)device;

@end

extern DKAxis *gDeviceAxis;

#endif /* KRX_DEV_DKAXIS_H */
