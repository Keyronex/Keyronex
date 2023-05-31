#import <KeyronexKit/KXDecoder.h>
#import <KeyronexKit/KXEncoder.h>
#import <ObjFW/OFString.h>
#import <ObjFWRT/ObjFWRT.h>
#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "encoding.h"

@implementation KXDecoder

@synthesize connection = _connection;

- (instancetype)initWithConnection:(KXIPCConnection *)connection
			       dxf:(dxf_t *)dxf
{
	self = [super init];
	_dxf = dxf;
	self.connection = connection;
	return self;
}

- (void)doDecodeValueOfObjCType:(const char *)type at:(void *)location
{
	switch (*type) {
	case _C_ID: {
		const char *name;
		Class cls;
		id result;

		assert(dxf_type(_dxf) == kDXFTypeDict);
		name = dxf_dict_get_string(_dxf, "#KX.class");

		if (strcmp(name, "KXIPCRemoteObject") == 0) {
			uint64_t num = dxf_dict_get_u64(_dxf, "proxyNumber");
			if (num == 0) {
				*(id *)location = _connection->_object;
				break;
			}
		}

		cls = objc_getClass(name);

		result = [[cls alloc] initWithCoder:self];

		*(id *)location = result;

		break;
	}

	case _C_SEL: {
		assert(dxf_type(_dxf) == kDXFTypeStr);
		*(SEL *)location = sel_registerName(
		    dxf_string_get_string_ptr(_dxf));
		break;
	}

	case _C_CHARPTR:
		assert(dxf_type(_dxf) == kDXFTypeStr);
		*(const char **)location = dxf_string_get_string_ptr(_dxf);
		break;

	case _C_ULNG_LNG: {
		assert(dxf_type(_dxf) == kDXFTypeU64);
		*(uint64_t *)location = dxf_u64_get_value(_dxf);
		break;
	}

	default:
		err(EXIT_FAILURE, "Can't decode type <%s>\n", type);
	}
}

- (void)decodeValueOfObjCType:(const char *)type
			   at:(void *)location
		       forKey:(const char *)key
{
	dxf_t *obj, *old;
	assert(key != NULL);

	obj = dxf_dict_get_value(_dxf, key);
	assert(obj != NULL);
	old = _dxf;
	_dxf = obj;
	[self doDecodeValueOfObjCType:type at:location];
	_dxf = old;
}

- (void)decodeValueOfObjCType:(const char *)type
			   at:(void *)location
	     atArraySubscript:(size_t)index
		       forKey:(const char *)key
{
	dxf_t *array, *obj, *old;

	array = dxf_dict_get_value(_dxf, key);
	assert(array && dxf_type(array) == kDXFTypeArray);

	obj = dxf_array_get_value(array, index);
	assert(obj);

	old = _dxf;
	_dxf = obj;
	[self doDecodeValueOfObjCType:type at:location];
	_dxf = old;
}

- (id)decodeObject;
{
	id object;
	[self doDecodeValueOfObjCType:@encode(id) at:&object];
	return object;
}

@end
