/*
 * Copyright (c) 2024 NetaScale Object Solutions.
 * Created on Fri Nov 01 2024.
 */
/*
 * @file OSObject.m
 * @brief Implements OSObject - the libkern base class.
 */

#include <kdk/kern.h>
#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <kdk/misc.h>

#include <libkern/OSObject.h>

#include <objfwrt/ObjFWRT.h>

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

struct osobject_header {
	uint32_t reference_count;
	uint32_t total_size;
};

#define OSOBJECT_HEADER_TOTAL_SIZE \
	ROUNDUP(sizeof(struct osobject_header), alignof(max_align_t))
#define OBJECT_HEADER(OBJ) \
	((struct osobject_header *)((char *)OBJ - OSOBJECT_HEADER_TOTAL_SIZE))

id
object_alloc(Class class)
{
	struct osobject_header *header;
	OSObject *object;
	size_t object_size;
	size_t total_size;

	object_size = class_getInstanceSize(class);
	total_size = OSOBJECT_HEADER_TOTAL_SIZE + object_size;

	header = kmem_zalloc(total_size);
	if (header == NULL)
		return nil;

	header->reference_count = 1;
	header->total_size = total_size;

	object = (OSObject *)((char *)header + OSOBJECT_HEADER_TOTAL_SIZE);
	if (!objc_constructInstance(class, object)) {
		kmem_free(header, total_size);
		return nil;
	}

	return object;
}

@implementation OSObject

+ (instancetype)alloc
{
	return object_alloc(self);
}

+ (instancetype)new
{
	return [[self alloc] init];
}

+ (void)load
{
}

+ (void)unload
{
}

+ (void)initialize
{
}

+ (Class)class
{
	return self;
}

+ (const char *)className
{
	return class_getName(self);
}

- (void)dealloc
{
	objc_destructInstance(self);
	kmem_free(OBJECT_HEADER(self), OBJECT_HEADER(self)->total_size);
}

- (instancetype)init
{
	return self;
}

- (instancetype)retain
{
	__atomic_fetch_add(&OBJECT_HEADER(self)->reference_count, 1,
	    __ATOMIC_RELAXED);
	return self;
}

- (void)release
{
	if (__atomic_fetch_sub(&OBJECT_HEADER(self)->reference_count, 1,
		__ATOMIC_RELEASE) == 1) {
		[self dealloc];
	}
}

- (const char *)className
{
	return object_getClassName(self);
}

- (bool)isEqual:(id)object
{
	return self == object;
}

- (bool)isKindOfClass:(Class)class
{
	for (Class c = object_getClass(self); c != nil;
	     c = class_getSuperclass(c)) {
		if (c == class)
			return true;
	}
	return false;
}

- (uintptr_t)hash;
{
	return (uintptr_t)self;
}

@end
