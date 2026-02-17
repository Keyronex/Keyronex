/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file DKPlatformRoot.h
 * @brief Platform-related protocols.
 */

#ifndef ECX_DEVICEKIT_DKPLATFORMROOT_H
#define ECX_DEVICEKIT_DKPLATFORMROOT_H

#include <keyronex/intr.h>
#include <stdint.h>

@class DKDevice;
@class DKPCIBridge;

@protocol DKPlatformRoot

- (int)routePCIPin:(uint8_t)pin
	 forBridge:(DKPCIBridge *)bridge
	      slot:(uint8_t)slot
	  function:(uint8_t)fun
	      into:(out kirq_source_t *)source;

- (int)handleSource:(struct kirq_source *)source
	withHandler:(kirq_handler_t *)handler
	   argument:(void *)arg
	  irqObject:(kirq_t *)object
	 atPriority:(ipl_t *)ipl;

@end

extern DKDevice<DKPlatformRoot> *gPlatformRoot;

#endif /* ECX_DEVICEKIT_DKPLATFORMROOT_H */
