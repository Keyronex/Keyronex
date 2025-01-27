/*
 * Copyright (c) 2024-2025 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSDictionary.c
 * @brief Libkern dictionary class implementation.
 */

#include <kdk/kmem.h>
#include <kdk/queue.h>
#include <libkern/OSDictionary.h>
#include <libkern/OSMapTable.h>

uintptr_t
OSDictionaryHash(const void *key)
{
	return [(id)key hash];
}

bool
OSDictionaryIsEqual(const void *key1, const void *key2)
{
	return [(id)key1 isEqual:(id)key2];
}

void *
OSDictionaryRetain(const void *key)
{
	return [(id)key retain];
}

void
OSDictionaryRelease(void *key)
{
	[(id)key release];
}

struct OSMapKeyCallbacks OSDictionaryKeyCallbacks = {
	.hash = OSDictionaryHash,
	.isEqual = OSDictionaryIsEqual,
	.retain = OSDictionaryRetain,
	.release = OSDictionaryRelease,
};

struct OSMapValueCallbacks OSDictionaryValueCallbacks = {
	.retain = OSDictionaryRetain,
	.release = OSDictionaryRelease,
};

@implementation OSDictionary

- (id)init
{
	if ((self = [super init])) {
		m_table = OSMapTableCreate(&OSDictionaryKeyCallbacks,
		    &OSDictionaryValueCallbacks);
	}
	return self;
}

- (void)dealloc
{
	OSMapTableDestroy(m_table);
	[super dealloc];
}

- (void)setObject:(id)object forKey:(id)key
{
	OSMapTableSetValue(m_table, key, object);
}

- (id)objectForKey:(id)key
{
	return OSMapTableGetValue(m_table, key);
}

- (size_t)count
{
	return OSMapTableGetCount(m_table);
}

@end
