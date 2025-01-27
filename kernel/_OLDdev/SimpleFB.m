
#include "dev/FBTerminal.h"
#include "dev/SimpleFB.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

static int counter = 0;

@interface SimpleFB (Private)
- (instancetype)initWithProvider:(DKDevice *)provider
			 address:(paddr_t)addr
			   width:(int)width
			  height:(int)height
			   pitch:(int)pitch;
@end

@implementation SimpleFB

+ (BOOL)probeWithProvider:(DKDevice *)provider
		  address:(paddr_t)addr
		    width:(int)width
		   height:(int)height
		    pitch:(int)pitch
{
	[[self alloc] initWithProvider:provider
			       address:addr
				 width:width
				height:height
				 pitch:pitch];
	return YES;
}

- (instancetype)initWithProvider:(DKDevice *)provider
			 address:(paddr_t)addr
			   width:(int)width
			  height:(int)height
			   pitch:(int)pitch
{
	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "simplefb-%d", counter++);

	m_info.address = addr;
	m_info.height = height;
	m_info.width = width;
	m_info.pitch = pitch;

	[self registerDevice];
	DKLogAttach(self);


	[FBTerminal probeWithFramebuffer:self];

	return self;
}

@end
