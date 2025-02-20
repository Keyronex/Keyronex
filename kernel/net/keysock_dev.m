#if 0 /* DK refactoring */

#include "ddk/DKDevice.h"
#include "kdk/dev.h"
#include "kdk/kmem.h"
#include "kdk/object.h"
#include "keysock_dev.h"

void *keysock_dev = NULL;

/* net/net.c */
void net_init(void);

iop_return_t keysock_dispatch_iop(iop_t *iop);

@implementation KeySock

- (instancetype)initWithProvider:(DKDevice *)provider
{
	self = [super initWithProvider:provider];
	kmem_asprintf(obj_name_ptr(self), "keysock");
	net_init();
	[self registerDevice];
	DKLogAttach(self);
	keysock_dev = self;
	return self;
}

+ (BOOL)probeWithProvider:(DKDevice *)provider
{
	kassert(keysock_dev == NULL);
	return [[self alloc] initWithProvider:provider] != nil;
}

- (iop_return_t)dispatchIOP:(iop_t *)iop
{
	return keysock_dispatch_iop(iop);
}

@end

#endif
