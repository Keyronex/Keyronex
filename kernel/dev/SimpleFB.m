/*
 * Copyright (c) 2023-2025 NetaScale Object Solutions.
 * Created on Thu Sep 14 2025.
 */

#include <dev/FBTerminal.h>
#include <dev/SimpleFB.h>

#include <ddk/DKAxis.h>
#include <kdk/kmem.h>

static int counter = 0;

@implementation SimpleFB


- (instancetype)initWithAddress:(paddr_t)addr
			   width:(int)width
			  height:(int)height
			   pitch:(int)pitch
{
	self = [super init];

	kmem_asprintf(&m_name, "simplefb-%d", counter++);

	m_info.address = addr;
	m_info.height = height;
	m_info.width = width;
	m_info.pitch = pitch;

	return self;
}

- (void)start
{
	FBTerminal *terminal = [[FBTerminal alloc] initWithFramebuffer:self];
	[self attachChild:terminal onAxis:gDeviceAxis];
	[terminal start];
}

@end
