/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2025-2026 Cloudarox Solutions.
 */
/*!
 * @file DKAxis.m
 * @brief DeviceKit axis class implementation.
 */

#include <keyronex/dlog.h>
#include <keyronex/kmem.h>
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

- (id)initWithName:(const char *)name
{
	if ((self = [super init])) {
		m_name = name;
		m_parents = [OSDictionary new];
		m_children = [OSDictionary new];
	}
	return self;
}

+ (instancetype )axisWithName:(const char *)name
{
	return [[DKAxis alloc] initWithName:name];
}

- (void)addChild:(DKDevice *)child ofParent:(DKDevice *)parent
{
	OSArray *children, *parents;

	ke_rwlock_enter_write(&gAxisLock);

	if (parent != nil) {
		children = [m_children objectForKey:parent];
		if (children == nil) {
			children = [OSArray new];
			[m_children setObject:children forKey:parent];
			[children release];
		}
		[children addObject:child];

		parents = [m_parents objectForKey:child];
		if (parents == nil) {
			parents = [OSArray new];
			[m_parents setObject:parents forKey:child];
			[parents release];
		}
		[parents addObject:parent];
	} else {
		m_root = [child retain];
	}

	ke_rwlock_exit_write(&gAxisLock);
}

- (OSArray *)childrenOf:(DKDevice *)device
{
	return [m_children objectForKey:device];
}

- (OSArray *)parentsOf:(DKDevice *)device
{
	return [m_parents objectForKey:device];
}

enum nodeKind { kRoot, kChild, kLastChild };

static void
printDeviceSubtree(DKDevice *dev, DKAxis *axis, char *prefix,
    enum nodeKind kind)
{
#if 0
	const char *branch = "+-";
	const char *rcorner = "\\-";
	const char *vline = "| ";
#else
	const char *branch = "\e(0\x74\x71\e(B";  /* ├─ */
	const char *rcorner = "\e(0\x6d\x71\e(B"; /* └─ */
	const char *vline = "\e(0\x78\e(B";	  /* │ */
#endif
	DKDevice *child;
	OSArray *children;
	char *newPrefix;

	if (kind == kRoot) {
		newPrefix = prefix;
	} else if (kind == kLastChild) {
		kdprintf("%s%s", prefix, rcorner);
		kmem_asprintf(&newPrefix, "%s%s", prefix, "  ");
	} else if (kind == kChild) {
		kdprintf("%s%s", prefix, branch);
		kmem_asprintf(&newPrefix, "%s%s ", prefix, vline);
	}

	kdprintf(kAnsiYellow "%s" kAnsiReset " (class %s)\n", [dev name],
	    [dev className]);

	children = [axis->m_children objectForKey:dev];
	for (size_t i = 0; i < [children count]; i++) {
		child = [children objectAtIndex:i];
		printDeviceSubtree(child, axis, newPrefix,
		    i == [children count] - 1 ? kLastChild : kChild);
	}

	if (newPrefix != prefix) {
		kmem_strfree(newPrefix);
	}
}

- (void)printSubtreeOfDevice:(DKDevice *)device
{
	if (device == nil) {
		kdprintf("(null device)\n");
		return;
	}

	kdprintf("Device tree rooted at " kAnsiYellow "%s" kAnsiReset
		" on the " kAnsiYellow "%s" kAnsiReset " axis:\n ",
	    [device name], m_name);

	printDeviceSubtree(device, self, "", kRoot);
}

@end
