#ifndef _KXIPCCONNECTION_H
#define _KXIPCCONNECTION_H

#import <ObjFW/OFObject.h>

#include "KXInvocation.h"

@interface KXIPCConnection : OFObject {
    @public
	int _fd;
	id _object;
	Protocol *_protocol;
	size_t _seqno;
}

/* server-side */
- (instancetype)initWithFD:(int)fd protocol:(Protocol *)protocol object:object;
/* client-side */
- (instancetype)initWithUnixSocketPath:(const char *)path
			      protocol:(Protocol *)protocol;
- (id)remoteObjectProxy;
- (void)sendInvocation:(KXInvocation *)invocation;

@end

#endif /* _KXIPCCONNECTION_H */
