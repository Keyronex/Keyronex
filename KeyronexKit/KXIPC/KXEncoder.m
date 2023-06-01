#import <KeyronexKit/KXEncoder.h>
#import <ObjFWRT/ObjFWRT.h>
#include <assert.h>
#include <err.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dxf/dxf.h"
#include "encoding.h"

@interface
KXEncoder (Private)
- (dxf_t *)doEncodeValueOfObjCType:(const char *)type at:(const void *)location;
@end

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

- (dxf_t *)doEncodePointerOfObjCType:(const char *)type
				  at:(void *const *)location
{
	dxf_t *dxf = [self doEncodeValueOfObjCType:type at:*location];
	return dxf;
}

- (dxf_t *)doEncodeValueOfObjCType:(const char *)type at:(const void *)location
{
	dxf_t *result;

retry:
	switch (*type) {
	case _C_CONST: {
		type++;
		goto retry;
	}

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

#define INTEGRAL_CASE(CTYPE)                      \
	{                                         \
		CTYPE value = *(CTYPE *)location; \
		result = dxf_create_u64(value);   \
		break;                            \
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
		result = dxf_create_u64(*(uint64_t *)location);
		break;
	}

	case _C_PTR: {
		result = [self doEncodePointerOfObjCType:type + 1 at:location];
		if (result)
			break;
		/* fallthrough */
	}

	case _C_STRUCT_B: {
		uintptr_t item;
		const char *subtype;
		result = dxf_create_array();

		/* pointer to first element of struct */
		item = (uintptr_t)location;
		/* skip struct name; xxx is that always present? */
		subtype = strchr(type, '=') + 1;

		for (const char *next_subtype; *subtype != _C_STRUCT_E;
		     subtype = next_subtype) {
			size_t size, align;
			dxf_t *subdxf;

			next_subtype = OFGetSizeAndAlignment(subtype, &size,
			    &align);
			if (align == 0)
				align = 1;
			item = ROUNDUP(item, align);
			subdxf = [self
			    doEncodeValueOfObjCType:subtype
						 at:(const void *)item];
			if (subdxf == NULL) {
				printf("encoding of struct subtype %s failed\n",
				    subtype);
				dxf_release(result);
				return NULL;
			}

			dxf_array_append_value(result, subdxf);
			dxf_release(subdxf);

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
		result = dxf_create_array();

		for (size_t i = 0; i < count; i++) {
			dxf_t *subdxf = [self
			    doEncodeValueOfObjCType:item_type
						 at:(const void *)item];
			if (subdxf == NULL) {
				dxf_release(result);
				return NULL;
			}
			dxf_array_append_value(result, subdxf);
			dxf_release(subdxf);

			item += sub_size;
		}

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
