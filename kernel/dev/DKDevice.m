#include "ddk/DKDevice.h"
#include "kdk/dev.h"
#include "kdk/object.h"

id ob_object_alloc(Class class, obj_class_t obclass);

obj_class_t device_class;

@implementation DKDevice

+ (void)load
{
	device_class = obj_new_type("Device");
}

+ (instancetype)alloc
{
	return ob_object_alloc(self, device_class);
}

+ (const char *)devName
{
	return [self className];
}

- (const char *)devName
{
	return obj_name(self);
}

- (DKDevice *)provider
{
	return m_provider;
}

- (void)addToTree
{
	TAILQ_INIT(&m_subDevices);
	if (m_provider)
		TAILQ_INSERT_TAIL(&m_provider->m_subDevices, self,
		    m_subDevices_entry);
}

- (id)initWithProvider:(DKDevice *)provider
{
	self = [super init];
	if (!self)
		return NULL;

	m_provider = [provider retain];

	return self;
}

- (void)registerDevice
{
	[self addToTree];
}

- (iop_return_t)completeIOP:(iop_t *)iop
{
	return kIOPRetCompleted;
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	kfatal("Unhandled dispatchIOP");
	return kIOPRetContinue;
}

@end
