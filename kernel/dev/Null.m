#include "Null.h"
#include "ddk/DKDevice.h"
#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/object.h"

static Null *null;

@implementation Null : DKDevice

+ (BOOL)probeWithProvider:(DKDevice *)provider
{
	return [[self alloc] initWithProvider:provider];
}

+ (instancetype)null
{
	return null;
}

- (instancetype)initWithProvider:(DKDevice *)provider
{
	self = [super initWithProvider:provider];

	kmem_asprintf(obj_name_ptr(self), "null");
	[self registerDevice];
	DKLogAttach(self);
	null = self;

	return self;
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	iop_frame_t *frame = iop_stack_current(iop);

	switch (frame->function) {
	case kIOPTypeWrite:
	case kIOPTypeRead:
		iop->result.result = -1;
		iop->result.count = frame->rw.bytes;

	default:
		kfatal("unimplemented\n");
	}

	return kIOPRetCompleted;
}

@end
