/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSString.m
 * @brief Libkern string class implementation.
 */

#include <kdk/libkern.h>

#import <libkern/OSString.h>

uint64_t MurmurHash64A(const void *key, int len, uint64_t seed);

@implementation OSString

- (bool)isEqual:(id)object
{
	OSString *other;

	if (object == self)
		return true;
	else if (![object isKindOfClass:[OSString class]])
		return false;

	other = object;

	if (length != other->length)
		return false;

	return memcmp(string, other->string, length) == 0;
}

- (uintptr_t)hash
{
	return MurmurHash64A(string, length, 0);
}

@end

@implementation OSConstantString
@end
