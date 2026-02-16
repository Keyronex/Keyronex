/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file DKDevice.h
 * @brief Device base class.
 */

#ifndef ECX_DEVICEKIT_DKDEVICE_H
#define ECX_DEVICEKIT_DKDEVICE_H

#include <devicekit/DKDevice.h>

#include <libkern/objc/OSObject.h>
#include <libkern/queue.h>

#define kAnsiYellow "\e[0;33m"
#define kAnsiReset "\e[0m"

@class DKAxis;

@interface DKDevice: OSObject {
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

#endif /* ECX_DEVICEKIT_DKDEVICE_H */
