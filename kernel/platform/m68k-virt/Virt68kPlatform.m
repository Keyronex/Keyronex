#include <ddk/DKInterrupt.h>
#include <ddk/DKPlatformRoot.h>

#include "bootinfo.h"
#include "ddk/DKDevice.h"
#include "ddk/virtio_mmio.h"
#include "dev/virtio/VirtIOMMIOTransport.h"
#include "kdk/endian.h"
#include "kdk/kern.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

extern Class gPlatformSpecificRootClass;

@interface Virt68kPlatform : DKDevice <DKPlatformRoot>

@end

@implementation Virt68kPlatform

+ (void)load
{
	gPlatformSpecificRootClass = self;
}

+ (BOOL)probe
{
	[[self alloc] init];
	return YES;
}

- (instancetype)init
{
	self = [super init];
	kmem_asprintf(&m_name, "virt68k-platform");
#if 0
	DKLogAttachExtra(self, "QEMU v%zu.%zu.%zu",
	    bootinfo.qemu_version >> 24 & 0xff,
	    bootinfo.qemu_version >> 16 & 0xff,
	    bootinfo.qemu_version >> 8 & 0xff);
#endif

	return self;
}

- (void)start
{
	volatile uint8_t *virtio_base = (void *)0xff010000;

	for (int i = 0; i < 128; i++) {
		[DKVirtIOMMIOTransport probeWithMMIO:virtio_base + 0x200 * i
					   interrupt:32 + i];
	}
}

- (DKPlatformInterruptController *)platformInterruptController
{
	return nil;
}

@end

void
platform_init()
{
	[Virt68kPlatform probe];
}
