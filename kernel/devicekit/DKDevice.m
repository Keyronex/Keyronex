//#include "dev/PCIBus.h"
#include "devicekit/DKDevice.h"

@implementation DKDevice

@synthesize name = m_name;
@synthesize provider = m_provider;

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

	self->m_provider = provider;

	return self;
}

- (void)registerDevice
{
	[self addToTree];
}

@end
