/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */
/*
 * @file DKAxis.m
 * @brief Implements DKAxis - the DeviceKit axis class.
 */

#include <ddk/DKAxis.h>
#include <kdk/kern.h>
#include <libkern/OSArray.h>
#include <libkern/OSDictionary.h>

DKAxis *gDeviceAxis;
static kmutex_t gAxisLock;

@implementation DKAxis

+ (void)load
{
	gDeviceAxis = [DKAxis axisWithName:"Device"];
	ke_mutex_init(&gAxisLock);
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

	ke_wait(&gAxisLock, __PRETTY_FUNCTION__, false, false, -1);

	children = [m_parents objectForKey:parent];
	if (children == nil) {
		children = [OSArray new];
		[m_parents setObject:children forKey:parent];
		[children release];
	}
	[children addObject:child];

	parents = [m_children objectForKey:child];
	if (parents == nil) {
		parents = [OSArray new];
		[m_children setObject:parents forKey:child];
		[parents release];
	}
	[parents addObject:parent];

	ke_mutex_release(&gAxisLock);
}

@end
