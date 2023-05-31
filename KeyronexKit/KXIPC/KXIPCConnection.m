#include <sys/socket.h>
#include <sys/un.h>

#import <KeyronexKit/KXDecoder.h>
#import <KeyronexKit/KXEncoder.h>
#import <KeyronexKit/KXIPCConnection.h>
#import <KeyronexKit/KXIPCRemoteObject.h>
#import <KeyronexKit/KXInvocation.h>
#include <assert.h>
#include <err.h>
#include <unistd.h>

@implementation KXIPCConnection

- (instancetype)initWithUnixSocketPath:(const char *)path
			      protocol:(Protocol *)protocol
{
	struct sockaddr_un sun;
	int r;

	self = [super init];

	_protocol = protocol;
	_object = nil;

	_fd = socket(PF_UNIX, SOCK_SEQPACKET, 0);
	assert(_fd >= 0);

	sun.sun_family = AF_UNIX;
	strcpy(sun.sun_path, path);

	r = connect(_fd, (struct sockaddr *)&sun, sizeof(sun));
	if (r < 0) {
		err(EXIT_FAILURE, "connect");
	}

	return self;
}

- (instancetype)initWithFD:(int)fd protocol:(Protocol *)protocol object:object
{
	self = [super init];

	_fd = fd;
	_protocol = protocol;
	_object = object;

	return self;
}

- (id)remoteObjectProxy
{
	return [[KXIPCRemoteObject alloc] initWithConnection:self
						 proxyNumber:0];
}

- (void)sendInvocation:(KXInvocation *)invocation
{
	dxf_t *dxf;
	KXEncoder *encoder = [[KXEncoder alloc] init];
	void *buf;
	ssize_t size;

	[encoder encodeObject:invocation];
	dxf = [encoder take];
	[encoder release];

	size = dxf_pack(dxf, &buf, NULL);
	assert(size > 0);

	dxf_release(dxf);

	size = write(_fd, buf, size);
	assert(size > 0);

	size = read(_fd, buf, 4096);
	assert(size > 0);

	dxf = dxf_unpack(buf, size, NULL);
	assert(dxf != NULL);

	dxf_dump(dxf);

	errx(EXIT_FAILURE, "Implement...\n");
}

- (void)handleMessage:(dxf_t *)dxf
{
	KXDecoder *decoder;
	KXInvocation *obj;

	printf("Received message:\n---\n");
	dxf_dump(dxf);
	printf("---\n");

	decoder = [[KXDecoder alloc] initWithConnection:self dxf:dxf];
	obj = [decoder decodeObject];

	assert([obj isKindOfClass:[KXInvocation class]]);
	[obj invoke];
}

@end
