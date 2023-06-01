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
retry:
	switch (*type) {
	case _C_CONST:
		type++;
		goto retry;

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

#define INTEGRAL_CASE(CTYPE)                                  \
	{                                                     \
		assert(dxf_type(_dxf) == kDXFTypeU64);        \
		*(CTYPE *)location = dxf_u64_get_value(_dxf); \
		break;                                        \
	}
	case _C_CHR:
		INTEGRAL_CASE(char);
	case _C_UCHR:
		INTEGRAL_CASE(unsigned char);
	case _C_SHT:
		INTEGRAL_CASE(short);
	case _C_USHT:
		INTEGRAL_CASE(unsigned short);
	case _C_INT:
		INTEGRAL_CASE(int);
	case _C_UINT:
		INTEGRAL_CASE(unsigned int);
	case _C_LNG:
		INTEGRAL_CASE(long);
	case _C_ULNG:
		INTEGRAL_CASE(unsigned long);
	case _C_LNG_LNG:
		INTEGRAL_CASE(long long);
#undef INTEGRAL_CASE

	case _C_ULNG_LNG: {
		assert(dxf_type(_dxf) == kDXFTypeU64);
		*(uint64_t *)location = dxf_u64_get_value(_dxf);
		break;
	}

	case _C_STRUCT_B: {
		uintptr_t item;
		const char *subtype;
		size_t i = 0;
		dxf_t *old;

		assert(dxf_type(_dxf) == kDXFTypeArray);

		/* pointer to first element of struct */
		item = (uintptr_t)location;
		/* skip struct name; xxx is that always present? */
		subtype = strchr(type, '=') + 1;

		for (const char *next_subtype; *subtype != _C_STRUCT_E;
		     subtype = next_subtype) {
			size_t size, align;

			next_subtype = OFGetSizeAndAlignment(subtype, &size,
			    &align);
			if (align == 0)
				align = 1;

			item = ROUNDUP(item, align);

			old = _dxf;
			_dxf = dxf_array_get_value(_dxf, i++);
			[self doDecodeValueOfObjCType:subtype at:(void *)item];
			_dxf = old;

			item += size;
		}

		break;
	}

	case _C_ARY_B: {
		const char *item_type;
		size_t count;
		size_t sub_size;
		uintptr_t item;

		/* pointer to first element of array */
		item = (uintptr_t)location;
		count = strtol(type + 1, (char **)&item_type, 10);
		OFGetSizeAndAlignment(item_type, &sub_size, NULL);

		if (isIntegralType(item_type)) {
			memcpy(location, dxf_bytes_get_data_ptr(_dxf),
			    sub_size * count);
			break;
		}

		for (size_t i = 0; i < count; i++) {
			dxf_t *old = _dxf;
			_dxf = dxf_array_get_value(_dxf, i);
			[self doDecodeValueOfObjCType:item_type
						   at:(void *)item];
			_dxf = old;
			item += sub_size;
		}

		break;
	}

	case _C_PTR: {
		void *val;
		size_t size;
		size_t align;

		OFGetSizeAndAlignment(type + 1, &size, &align);
		val = malloc(size);
		[self doDecodeValueOfObjCType:type + 1 at:val];

		*(void **)location = val;
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
