/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */
/*!
 * @file DKDevice.h
 * @brief DeviceKit device class and related definitions.
 */

#ifndef KRX_DEV_DKDEVICE_H
#define KRX_DEV_DKDEVICE_H

#include <libkern/OSObject.h>
#include <kdk/queue.h>

#define kAnsiYellow "\e[0;33m"
#define kAnsiReset  "\e[0m"

@class DKAxis;

/*!
 * @brief DeviceKit device class.
 */
@interface DKDevice : OSObject {
	char *m_name;
	TAILQ_TYPE_ENTRY(DKDevice) m_queue_link;
}

@property (readonly) const char *name;

+ (void)drainStartQueue;
- (void)addToStartQueue;
- (void)start;

- (void)attachChild:(DKDevice *)child onAxis:(DKAxis *)axis;

@end

typedef TAILQ_TYPE_HEAD(dk_device_queue, DKDevice) dk_device_queue_t;

#endif /* KRX_DEV_DKDEVICE_H */
