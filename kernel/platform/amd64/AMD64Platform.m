#include "ddk/DKDevice.h"
#include "dev/acpi/DKAACPIPlatform.h"
#include "dev/SimpleFB.h"
#include "dev/Null.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "limine.h"
#include "net/keysock_dev.h"
#include "platform/amd64/IOAPIC.h"

extern volatile struct limine_framebuffer_request framebuffer_request;

@interface VirtAMD64Platform : DKDevice <DKPlatformDevice>

@end

extern struct bootinfo bootinfo;

@implementation VirtAMD64Platform

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
	extern struct limine_rsdp_request rsdp_request;
	struct limine_framebuffer *fb =
	    framebuffer_request.response->framebuffers[0];

	self = [super init];
	kmem_asprintf(obj_name_ptr(self), "amd64-platform");
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
	[KeySock probeWithProvider:self];
	[Null probeWithProvider:self];
	[[DKACPIPlatform instance] secondStageInit];
}

- (DKDevice<DKPlatformInterruptControl> *)platformInterruptController
{
	return (id)[IOApic class];
}

@end
