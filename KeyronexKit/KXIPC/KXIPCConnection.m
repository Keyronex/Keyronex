#include <sys/socket.h>
#include <sys/un.h>

#import <KeyronexKit/KXDecoder.h>
#import <KeyronexKit/KXEncoder.h>
#import <KeyronexKit/KXIPCConnection.h>
#import <KeyronexKit/KXIPCRemoteObject.h>
#import <KeyronexKit/KXInvocation.h>
#include <KeyronexKit/macros.h>
#include <assert.h>
#include <err.h>
#include <unistd.h>

#include "encoding.h"

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
	dxf_t *dxf = dxf_create_dict(), *value;
	KXEncoder *encoder = [[KXEncoder alloc] init];
	void *buf;
	ssize_t size, actual;
	uint64_t sent_seqno = _seqno++;

	dxf_dict_set_string(dxf, "#kind", "invocation");
	dxf_dict_set_u64(dxf, "#sequence", sent_seqno);

	[encoder encodeObject:invocation];
	dxf_dict_movein_value(dxf, "#invocation", [encoder take]);
	[encoder release];

	size = dxf_pack(dxf, &buf, NULL);
	assert(size > 0);

	dxf_release(dxf);

	actual = write(_fd, buf, size);
	assert(actual == size);

	kmem_free(buf, size);

	/* handle reply */
	const char *return_type;
	KXDecoder *decoder;

	return_type = [[invocation methodSignature] methodReturnType];
	if (*return_type == 'V')
		return; /* oneway */

	buf = kmem_alloc(4096);

	size = read(_fd, buf, 4096);
	assert(size > 0);

	dxf = dxf_unpack(buf, size, NULL);
	assert(dxf != NULL);

	kmem_free(buf, 4096);

	uint64_t seqno = dxf_dict_get_u64(dxf, "#sequence");
	const char *kind = dxf_dict_get_string(dxf, "#kind");

	if (strcmp(kind, "return") != 0) {
		fatal("Bad return kind %s\n", kind);
	} else if (seqno != sent_seqno) {
		fatal("Mismatched reply seqno (got %lu, expected %lu)\n", seqno,
		    sent_seqno);
	}

	if (*return_type == _C_VOID)
		return;

	decoder = [[KXDecoder alloc]
	    initWithConnection:self
			   dxf:dxf_dict_get_value(dxf, "#return")];
	[decoder decodeValueOfObjCType:return_type
				    at:[invocation returnValuePtr]
				forKey:"returnValue"];
	[decoder release];
	dxf_release(dxf);
}

- (void)handleMessage:(dxf_t *)dxf
{
	KXDecoder *decoder;
	size_t seqno;
	const char *kind;

	printf("Received message:\n---\n");
	dxf_dump(dxf);
	printf("---\n");

	seqno = dxf_dict_get_u64(dxf, "#sequence");
	kind = dxf_dict_get_string(dxf, "#kind");

	if (strcmp(kind, "invocation") == 0) {
		KXEncoder *encoder;
		KXInvocation *invocation;
		dxf_t *dxf_invoc = dxf_dict_get_value(dxf, "#invocation"),
		      *dxf_return;
		const char *return_type;
		bool returns_value = true;
		void *buf;
		ssize_t size;

		decoder = [[KXDecoder alloc] initWithConnection:self
							    dxf:dxf_invoc];
		invocation = [decoder decodeObject];

		assert([invocation isKindOfClass:[KXInvocation class]]);
		[invocation invoke];

		return_type = [[invocation methodSignature] methodReturnType];

		if (*return_type == 'V') {
			/* oneway void */
			return;
		} else if (*return_type == 'v') {
			returns_value = false;
		}

		if (returns_value) {
			encoder = [[KXEncoder alloc] init];
			[encoder
			    encodeValueOfObjCType:[[invocation methodSignature]
						      methodReturnType]
					       at:[invocation returnValuePtr]
					   forKey:"returnValue"];
		}

		dxf_return = dxf_create_dict();
		dxf_dict_set_string(dxf_return, "#kind", "return");
		dxf_dict_set_u64(dxf_return, "#sequence", seqno);
		if (returns_value)
			dxf_dict_movein_value(dxf_return, "#return",
			    [encoder take]);

		size = dxf_pack(dxf_return, &buf, NULL);
		assert(size > 0);

		size = write(_fd, buf, size);
		assert(size > 0);

		kmem_free(buf, size);
	} else {
		printf("Unexpected msesage (%s)\n", kind);
		return;
	}
}

@end
