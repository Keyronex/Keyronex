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

#include <kdk/queue.h>
#include <kdk/vmtypes.h>
#include <libkern/OSObject.h>

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

int dk_allocate_and_map(vaddr_t *out_vaddr, paddr_t *out_paddr, size_t size);

#endif /* KRX_DEV_DKDEVICE_H */
