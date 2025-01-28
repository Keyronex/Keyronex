/*
 * Copyright (c) 2025 NetaScale Object Solutions.
 * Created on Mon Jan 27 2025.
 */
/*
 * @file OSArray.m
 * @brief Libkern array class implementation.
 */

#include <kdk/kmem.h>
#include <kdk/libkern.h>
#include <libkern/OSArray.h>

@implementation OSArray

- (id)init
{
	if ((self = [super init])) {
		_capacity = 16;
		_objects = kmem_alloc(sizeof(id) * _capacity);
		if (_objects == NULL) {
			[self release];
			return nil;
		}
		_count = 0;
	}
	return self;
}

- (void)dealloc
{
	for (size_t i = 0; i < _count; i++)
		[_objects[i] release];

	kmem_free(_objects, sizeof(id) * _capacity);
	[super dealloc];
}

- (void)addObject:(id)object
{
	if (object == nil)
		return;

	if (_count >= _capacity) {
		size_t newCapacity = _capacity * 2;
		id *newObjects = kmem_alloc(sizeof(id) * newCapacity);

		if (newObjects == NULL)
			return;

		memcpy(newObjects, _objects, sizeof(id) * _count);
		kmem_free(_objects, sizeof(id) * _capacity);
		_objects = newObjects;
		_capacity = newCapacity;
	}

	_objects[_count++] = [object retain];
}

- (void)insertObject:(id)object atIndex:(size_t)index
{
	if (object == nil || index > _count)
		return;

	if (_count >= _capacity) {
		size_t newCapacity = _capacity * 2;
		id *newObjects = kmem_alloc(sizeof(id) * newCapacity);
		if (newObjects == NULL)
			return;

		memcpy(newObjects, _objects, sizeof(id) * _count);
		kmem_free(_objects, sizeof(id) * _capacity);
		_objects = newObjects;
		_capacity = newCapacity;
	}

	if (index < _count) {
		memmove(&_objects[index + 1], &_objects[index],
		    sizeof(id) * (_count - index));
	}

	_objects[index] = [object retain];
	_count++;
}

- (void)removeObjectAtIndex:(size_t)index
{
	if (index >= _count)
		return;

	[_objects[index] release];

	if (index < _count - 1)
		memmove(&_objects[index], &_objects[index + 1],
		    sizeof(id) * (_count - index - 1));

	_count--;
}

- (void)removeLastObject
{
	if (_count > 0) {
		[self removeObjectAtIndex:_count - 1];
	}
}

- (void)removeAllObjects
{
	for (size_t i = 0; i < _count; i++) {
		[_objects[i] release];
	}
	_count = 0;
}

- (id)objectAtIndex:(size_t)index
{
	if (index >= _count)
		return nil;
	return _objects[index];
}

- (size_t)count
{
	return _count;
}

- (void)replaceObjectAtIndex:(size_t)index withObject:(id)object
{
	if (index >= _count || object == nil)
		return;

	[_objects[index] release];
	_objects[index] = [object retain];
}

- (bool)isEqual:(id)object
{
	OSArray *other;

	if (object == self)
		return true;

	if (![object isKindOfClass:[OSArray class]])
		return false;

	other = object;
	if (_count != other->_count)
		return false;

	for (size_t i = 0; i < _count; i++) {
		if (![_objects[i] isEqual:other->_objects[i]])
			return false;
	}

	return true;
}

- (uintptr_t)hash
{
	uintptr_t hash = 0;
	for (size_t i = 0; i < _count; i++)
		hash = hash * 31 + [_objects[i] hash];
	return hash;
}

- (int)countByEnumeratingWithState:(OSFastEnumerationState *)state
			   objects:(id __unsafe_unretained[])buffer
			     count:(int)len
{
	unsigned long index, count;

	/* On first call, initialize the state */
	if (state->state == 0) {
		state->mutationsPtr = (unsigned long *)self;
		state->state = 1;
		state->itemsPtr = (id __unsafe_unretained *)_objects;
		state->extra[0] = 0;
	}

	index = state->extra[0];
	count = MIN2(len, _count - index);

	if (count > 0) {
		state->itemsPtr = (id __unsafe_unretained *)&_objects[index];
		state->extra[0] = index + count;
		return count;
	} else {
		return 0;
	}
}

@end
