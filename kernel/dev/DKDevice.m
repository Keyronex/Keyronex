/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Wed Sep 20 2023.
 */
/*!
 * @file DKDevice.m
 * @brief DeviceKit device class.
 */

#include <ddk/DKAxis.h>
#include <ddk/DKDevice.h>
#include <kdk/kern.h>

@protocol DKPlatformRoot;

kspinlock_t gStartQueueLock;
dk_device_queue_t gStartQueue = TAILQ_HEAD_INITIALIZER(gStartQueue);
DKDevice<DKPlatformRoot> *gPlatformRoot;

void
DKLogAttach(DKDevice *child, DKDevice *parent)
{
	const char *dev_name;
	const char *prov_name;

	if ((dev_name = [child name]) == NULL)
		dev_name = [child className];

	if (parent == NULL)
		prov_name = "(root)";
	else if ((prov_name = [parent name]) == NULL)
		prov_name = [parent className];

	kprintf(kAnsiYellow "%s" kAnsiReset " at " kAnsiYellow "%s" kAnsiReset
			    "\n",
	    dev_name, prov_name);
}

@implementation DKDevice

- (const char *)name
{
	return m_name;
}

- (void)addToStartQueue
{
	ipl_t ipl = ke_spinlock_acquire(&gStartQueueLock);
	TAILQ_INSERT_TAIL(&gStartQueue, self, m_queue_link);
	ke_spinlock_release(&gStartQueueLock, ipl);
}

+ (void)drainStartQueue
{
	DKDevice *device;
	ipl_t ipl = ke_spinlock_acquire(&gStartQueueLock);
	while ((device = TAILQ_FIRST(&gStartQueue)) != NULL) {
		TAILQ_REMOVE(&gStartQueue, device, m_queue_link);
		ke_spinlock_release(&gStartQueueLock, ipl);
		[device start];
		ipl = ke_spinlock_acquire(&gStartQueueLock);
	}
	ke_spinlock_release(&gStartQueueLock, ipl);
}

- (void)attachChild:(DKDevice *)child onAxis:(DKAxis *)axis
{
	[axis addChild:child ofParent:self];
	if (axis == gDeviceAxis)
		DKLogAttach(child, self);
}

- (void)start
{
	/* epsilon */
}

@end
