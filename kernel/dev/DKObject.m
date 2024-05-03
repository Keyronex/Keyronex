#include <stdalign.h>
#include <stddef.h>

#include "ObjFWRT.h"
#include "ddk/DKObject.h"
#include "kdk/kmem.h"
#include "kdk/nanokern.h"
#include "kdk/libkern.h"
#include "kdk/object.h"

struct dkobject_header {
	uint32_t reference_count;
	uint32_t total_size;
};

#define DKOBJECT_HEADER_TOTAL_SIZE \
	ROUNDUP(sizeof(struct dkobject_header), alignof(max_align_t))
#define OBJECT_HEADER(OBJ) \
	((struct dkobject_header *)((char *)OBJ - DKOBJECT_HEADER_TOTAL_SIZE))

id
object_alloc(Class class)
{
	struct dkobject_header *header;
	DKObject *object;
	size_t object_size;
	size_t total_size;

	object_size = class_getInstanceSize(class);
	total_size = DKOBJECT_HEADER_TOTAL_SIZE + object_size;

	header = kmem_zalloc(total_size);
	if (header == NULL)
		return nil;

	header->reference_count = 1;
	header->total_size = total_size;

	object = (DKObject *)((char *)header + DKOBJECT_HEADER_TOTAL_SIZE);
	if (!objc_constructInstance(class, object)) {
		kmem_free(header, total_size);
		return nil;
	}

	return object;
}

@implementation DKObject

+ (instancetype)alloc
{
	return object_alloc(self);
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

@end

id
ob_object_alloc(Class class, obj_class_t obclass)
{
	DKOBObject *object;
	size_t object_size;
	int r;

	object_size = class_getInstanceSize(class);

	r = obj_new(&object, obclass, object_size, NULL);
	memset(object, 0x0, object_size);
	if (r != 0)
		return nil;

	if (!objc_constructInstance(class, object)) {
		/* need some object manager explicit delete call... */
		return nil;
	}

	return object;
}

@implementation DKOBObject

+ (instancetype)alloc
{
	kfatal("This must be overriden in subclasses to create an object "
	       "of the proper object manager class.\n");
}


- (void)dealloc
{
	objc_destructInstance(self);
	// obj_object_delete(object);

	/* silence GCC warning */
	if (false)
		[super dealloc];
}

- (instancetype)retain
{
	obj_retain(self);
	return self;
}

- (void)release
{
	obj_release(self);
}

@end
