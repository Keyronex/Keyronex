#include "ddk/DKDevice.h"
#include "dev/Null.h"
#include "dev/SimpleFB.h"
#include "kdk/kmem.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "kdk/object.h"
#include "limine.h"
#include "net/keysock_dev.h"

extern volatile struct limine_framebuffer_request framebuffer_request;

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

	struct limine_framebuffer *fb =
	    framebuffer_request.response->framebuffers[0];

	self = [super init];
	kmem_asprintf(obj_name_ptr(self), "riscv64-platform");
	platformDevice = self;
	[self registerDevice];
	DKLogAttach(self);

	[SimpleFB probeWithProvider:self
			    address:V2P(fb->address)
			      width:fb->width
			     height:fb->height
			      pitch:fb->pitch];
	[DKACPIPlatform probeWithProvider:self rsdp:rsdp_request.response->address];

	return self;
}

- (void)secondStageInit
{
	[Null probeWithProvider:self];
	[KeySock probeWithProvider:self];
	[[DKACPIPlatform instance] secondStageInit];
}

@end
