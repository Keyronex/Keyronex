/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Sat May 4 2024.
 */

#include <ddk/DKVirtIOTransport.h>

@implementation DKVirtIOTransport

- (volatile void *)deviceConfig
{
	kfatal("subclass responsibility\n");
}

- (void)resetDevice
{
	kfatal("subclass responsibility\n");
}

- (int)enableDevice
{
	kfatal("subclass responsibility\n");
}

- (bool)exchangeFeaturesMandatory:(uint64_t)mandatory
			 optional:(uint64_t *)optional
{
	kfatal("subclass responsibility\n");
}

- (void)setupQueue:(virtio_queue_t *)queue index:(uint16_t)index
{
	kfatal("subclass responsibility\n");
}

- (int)allocateDescNumOnQueue:(struct virtio_queue *)queue
{
	kfatal("subclass responsibility\n");
}

- (void)freeDescNum:(uint16_t)num onQueue:(struct virtio_queue *)queue
{
	kfatal("subclass responsibility\n");
}

- (void)submitDescNum:(uint16_t)descNum toQueue:(struct virtio_queue *)queue
{
	kfatal("subclass responsibility\n");
}

- (void)notifyQueue:(struct virtio_queue *)queue
{
	kfatal("subclass responsibility\n");
}

@end
