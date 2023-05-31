#import <KeyronexKit/KXEncoder.h>
#import <ObjFWRT/ObjFWRT.h>
#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>

#include "encoding.h"

@implementation KXEncoder

- (instancetype)init
{
	self = [super init];
	_dxf = dxf_create_dict();
	return self;
}

- (dxf_t *)take
{
	dxf_t *dxf = _dxf;
	_dxf = NULL;
	return dxf;
}

- (dxf_t *)doEncodeValueOfObjCType:(const char *)type at:(const void *)location
{
	dxf_t *result;

	switch (*type) {
	case _C_ID: {
		id object = *(id *)location;
		dxf_t *old = _dxf;

		_dxf = dxf_create_dict();
		dxf_dict_set_string(_dxf, "#KX.class",
		    class_getName([object class]));
		[object encodeWithCoder:self];
		result = _dxf;
		_dxf = old;

		break;
	}

	case _C_SEL: {
		result = dxf_create_str(sel_getName(*(SEL *)location));
		break;
	}

	case _C_CHARPTR:
		result = dxf_create_str(*(const char **)location);
		break;

	case _C_ULNG_LNG: {
		result = dxf_create_u64(*(uint64_t *)location);
		break;
	}

	default:
		err(EXIT_FAILURE, "Can't encode type <%s>\n", type);
	}

	return result;
}

- (void)encodeValueOfObjCType:(const char *)type
			   at:(const void *)location
		       forKey:(const char *)key
{
	dxf_t *result;
	assert(key != NULL);

	dxf_dict_set_null(_dxf, key);
	result = [self doEncodeValueOfObjCType:type at:location];
	assert(result != NULL);
	dxf_dict_set_value(_dxf, key, result);
	dxf_release(result);
}

- (void)encodeValuesOfObjCTypesInArray:(id<KXArrayEncoding>)object
				forKey:(const char *)key
{
	size_t limit = [object count];
	dxf_t *dxfarray;

	dxfarray = dxf_create_array();
	for (int i = 0; i < limit; i++) {
		const char *sig;
		const void *data;
		dxf_t *result;
		[object getElementLocation:&data andTypeSignature:&sig at:i];
		result = [self doEncodeValueOfObjCType:sig at:data];
		dxf_array_append_value(dxfarray, result);
		dxf_release(result);
	}
	dxf_dict_set_value(_dxf, key, dxfarray);
	dxf_release(dxfarray);
}

- (void)encodeObject:(id)anObject
{
	_dxf = [self doEncodeValueOfObjCType:@encode(id) at:&anObject];
}

- (void)dump
{
	printf("--\n");
	dxf_dump(_dxf);
	printf("\n--\n");
}

@end
