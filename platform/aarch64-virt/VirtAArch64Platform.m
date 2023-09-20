#include "ddk/DKDevice.h"
#include "kdk/kmem.h"
#include "dev/DKAACPIPlatform.h"
#include "limine.h"

@interface VirtAArch64Platform : DKDevice <DKPlatformDevice>

@end

extern struct bootinfo bootinfo;

@implementation VirtAArch64Platform

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
	extern struct limine_rsdp_request rsdp_request;

	self = [super init];
	kmem_asprintf(obj_name_ptr(self), "aarch64-virt-platform");
	platformDevice = self;
	[self registerDevice];
	DKLogAttach(self);

	[DKACPIPlatform probeWithProvider:self rsdp:rsdp_request.response->address];

	return self;
}

@end
