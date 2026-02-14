/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Cloudarox Solutions.
 */
/*!
 * @file dk.m
 * @brief DeviceKit entry.
 */

#include <keyronex/dlog.h>
#include <keyronex/kmem.h>

void abort(void)
{
	kfatal("abort() called");
}

void *
malloc(size_t size)
{
	return kmem_malloc(size);
}

void
free(void *ptr)
{
	return kmem_mfree(ptr);
}
