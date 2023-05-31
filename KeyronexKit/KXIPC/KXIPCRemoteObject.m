#import <ObjFW/OFMethodSignature.h>
#import <ObjFW/OFString.h>
#include <assert.h>

#import <KeyronexKit/KXEncoder.h>
#import <KeyronexKit/KXIPCRemoteObject.h>
#import <KeyronexKit/KXInvocation.h>

const char *_protocol_getMethodTypeEncoding(Protocol *p, SEL sel,
    BOOL isRequiredMethod, BOOL isInstanceMethod);

@implementation KXIPCRemoteObject

- (instancetype)initWithConnection:(KXIPCConnection *)conn
		       proxyNumber:(uint64_t)num
{
	self = [super init];
	_conn = conn;
	_num = num;
	return self;
}

- (instancetype)initWithCoder:(KXDecoder *)coder;
{
	assert([coder isKindOfClass:[KXDecoder class]]);
	self = [super init];
	_conn = coder.connection;
	[coder decodeValueOfObjCType:@encode(uint64_t)
				  at:&_num
			      forKey:"proxyNumber"];
	return self;
}

- (OFMethodSignature *)methodSignatureForSelector:(SEL)sel
{
	const char *code = _protocol_getMethodTypeEncoding(_conn->_protocol,
	    sel, true, true);
	assert(code != NULL);
	return [OFMethodSignature signatureWithObjCTypes:code];
}

- (void)forwardInvocation:(KXInvocation *)inv
{
	[_conn sendInvocation:inv];
}

- (void)encodeWithCoder:(KXEncoder *)encoder
{
	[encoder encodeValueOfObjCType:@encode(uint64_t)
				    at:&_num
				forKey:"proxyNumber"];
}

- (OFString *)description
{
	return [OFString
	    stringWithFormat:
		@"<%@ %p: proxyNumber=%lu>",
	    [self class], self, _num];
}


@end
