#include "bootinfo.h"
#include "ddk/DKDevice.h"
#include "ddk/virtio_mmio.h"
#include "dev/virtio/DKVirtIOMMIOTransport.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/kern.h"
#include "kdk/object.h"

@interface Virt68kPlatform : DKDevice <DKPlatformDevice>

@end

extern struct bootinfo bootinfo;

@implementation Virt68kPlatform

+ (void)load
{
	extern Class platformDeviceClass;
	platformDeviceClass = self;
}

+ (BOOL)probe
{
	[[self alloc] init];
	return YES;
}

- (instancetype)init
{
	self = [super init];
	kmem_asprintf(obj_name_ptr(self), "virt68k-platform");
	platformDevice = self;
	[self registerDevice];
	DKLogAttachExtra(self, "QEMU v%zu.%zu.%zu",
	    bootinfo.qemu_version >> 24 & 0xff,
	    bootinfo.qemu_version >> 16 & 0xff,
	    bootinfo.qemu_version >> 8 & 0xff);

	return self;
}

- (void)secondStageInit
{
	volatile uint8_t *virtio_base = (void *)bootinfo.virtio_base;

	for (int i = 0; i < 128; i++) {
		[DKVirtIOMMIOTransport probeWithProvider:self
						    mmio:virtio_base + 0x200 * i
					       interrupt:32 + i];
	}
}

- (DKDevice<DKPlatformInterruptControl> *)platformInterruptController
{
	return nil;
}

@end

void
platform_init()
{
	[Virt68kPlatform probe];
}
