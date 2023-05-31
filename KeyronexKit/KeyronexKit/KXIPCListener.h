#ifndef _KXIPCLISTENER_H
#define _KXIPCLISTENER_H

#import <ObjFW/OFDictionary.h>
#import <ObjFW/OFObject.h>

@interface KXIPCListener : OFObject {
	int _fd;
	id _object;
	Protocol *_protocol;
	OFMutableDictionary *_clients;
}

- (instancetype)initWithUnixSocketPath:(const char *)path
			      protocol:(Protocol *)protocol
				object:(id)object;
- (id)remoteObjectProxy;
- (void) run;

@end

#endif /* _KXIPCLISTENER_H */
