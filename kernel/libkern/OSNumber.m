/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSNumber.m
 * @brief Libkern number class implementation.
 */

#include <kdk/libkern.h>

#include <libkern/OSNumber.h>

@implementation OSNumber

+ (instancetype)numberWithUInt8:(uint8_t)value
{
	return [[self alloc] initWithUInt64:value withSize:sizeof(uint8_t)];
}

+ (instancetype)numberWithUInt16:(uint16_t)value
{
	return [[self alloc] initWithUInt64:value withSize:sizeof(uint16_t)];
}

+ (instancetype)numberWithUInt32:(uint32_t)value
{
	return [[self alloc] initWithUInt64:value withSize:sizeof(uint32_t)];
}

+ (instancetype)numberWithUInt64:(uint64_t)value
{
	return [[self alloc] initWithUInt64:value withSize:sizeof(uint64_t)];
}

- (instancetype)initWithUInt64:(uint64_t)val withSize:(uint32_t)bytes
{
	if ((self = [super init])) {
		uint64_t mask;

		if (bytes != 1 && bytes != 2 && bytes != 4 && bytes != 8) {
			[self release];
			return nil;
		}

		mask = (1ULL << (bytes * 8)) - 1;
		value = val & mask;
		size = bytes;
	}
	return self;
}

- (bool)isEqual:(id)object
{
	if (object == self)
		return true;

	if (![object isKindOfClass:[OSNumber class]])
		return false;

	OSNumber *other = object;
	return value == other->value && size == other->size;
}

- (uintptr_t)hash
{
	return (uintptr_t)value;
}

- (uint64_t)unsignedValue
{
	return value;
}

- (uint32_t)size
{
	return size;
}

@end
