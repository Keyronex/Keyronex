/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file DKAxis.m
 * @brief DeviceKit axis class implementation.
 */


#include <keyronex/ktask.h>

#include <devicekit/DKAxis.h>

DKAxis *gDeviceAxis;
krwlock_t gAxisLock;

@implementation DKAxis

+ (void)load
{
	gDeviceAxis = [DKAxis axisWithName:"Device"];
	ke_rwlock_init(&gAxisLock);
}

@end
