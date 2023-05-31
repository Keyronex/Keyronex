#ifndef _KXREMOTEOBJECT_H
#define _KXREMOTEOBJECT_H

#import <ObjFW/OFObject.h>
#import <KeyronexKit/KXIPCConnection.h>
#include <KeyronexKit/KXDecoder.h>


@interface KXIPCRemoteObject : OFObject {
	uint64_t _num;
	KXIPCConnection * _conn;
}

- (instancetype) initWithConnection: (KXIPCConnection *)conn proxyNumber: (uint64_t) num;
- (instancetype) initWithCoder:(KXDecoder *)coder;

@end

#endif /* _KXREMOTEOBJECT_H */
