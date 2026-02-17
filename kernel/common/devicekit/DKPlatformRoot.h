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

- (void)routePCIPin:(uint8_t)pin
	 forBridge:(DKPCIBridge *)bridge
	      slot:(uint8_t)slot
	  function:(uint8_t)fun
	      into:(out kirq_source_t *)source;

- (int)handleSource:(struct kirq_source *)source
	withHandler:(kirq_handler_t *)handler
	   argument:(void *)arg
	 atPriority:(ipl_t *)ipl
	  irqObject:(out kirq_t *)object;

@end

extern DKDevice<DKPlatformRoot> *gPlatformRoot;

#endif /* ECX_DEVICEKIT_DKPLATFORMROOT_H */
