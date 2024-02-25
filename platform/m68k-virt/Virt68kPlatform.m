#include "ddk/DKDevice.h"
#include "ddk/DKVirtIOMMIODevice.h"
#include "ddk/virtio_mmio.h"
#include "kdk/endian.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "kdk/nanokern.h"
#include "bootinfo.h"

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
	extern id platformDevice;
	volatile uint8_t *virtio_base = (void *)bootinfo.virtio_base;

	self = [super init];
	kmem_asprintf(obj_name_ptr(self), "virt68k-platform");
	platformDevice = self;
	[self registerDevice];
	DKLogAttachExtra(self, "QEMU v%zu.%zu.%zu",
	    bootinfo.qemu_version >> 24 & 0xff,
	    bootinfo.qemu_version >> 16 & 0xff,
	    bootinfo.qemu_version >> 8 & 0xff);

	for (int i = 0; i < 128; i++) {
		[DKVirtIOMMIODevice probeWithProvider:self
						 mmio:virtio_base + 0x200 * i
						 interrupt:32 + i];
	}

	return self;
}

@end

void
platform_init()
{
	[Virt68kPlatform probe];
}
